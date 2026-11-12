"""Export the sidecar replacement's super-resolution models to ONNX.

Exports each .pth checkpoint (loaded via spandrel, same as the reference
gpt-anime-denoise denoise.py) to a plain opset-17 ONNX graph in models/:

  input  "input"  NCHW float32 RGB in [0, 1], dynamic N/H/W
  output "output" NCHW float32, no clamp inside the graph

Weights stay fp32 and no external optimizer/simplifier is baked in, so the C
engine's ONNX Runtime sees the same maths as the torch reference.

Usage (run with a python that has torch+spandrel+onnx, e.g. the tmp export env):

  python tools/export_onnx.py                # export all three models
  python tools/export_onnx.py esrgan cugan   # only some

Downloads are cached in _dl/models-src/ and SHA-256 verified against the same
table as denoise.py; the CUGAN weight is extracted from its zip release.
Outputs land in models/ next to this repo's assets (gitignored, *.onnx).

For APISR and CUGAN, whose licenses do not let us redistribute the weights, the
tool also writes weightless skeletons next to the full models:

  apisr_rrdb_x2.skel.onnx
  cugan_up2x_denoise3x.skel.onnx

A skeleton is the same graph whose initializers carry ONNX "external data"
references (location = the upstream .pth file name, offset, length) straight
into the SHA-verified .pth bytes, so a user who downloads the checkpoint into
the models folder gets the exact weights with no conversion. The byte offsets
are found by parsing the checkpoint container (zip-local headers + data.pkl
for APISR; the legacy torch stream for CUGAN) and matched to initializers by
content hash, so a mismatched file is reported, not silently wired.
"""
import hashlib
import io
import pickletools
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / "_dl" / "models-src"
OUT = REPO / "models"

MODELS = {
    "esrgan": ("RealESRGAN_x4plus_anime_6B.pth",
               "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth",
               "esrgan_anime6b_x4.onnx"),
    "apisr": ("2x_APISR_RRDB_GAN_generator.pth",
              "https://huggingface.co/camenduru/APISR/resolve/main/2x_APISR_RRDB_GAN_generator.pth",
              "apisr_rrdb_x2.onnx"),
    "cugan": ("up2x-latest-denoise3x.pth",
              "https://github.com/bilibili/ailab/releases/download/Real-CUGAN/updated_weights.zip",
              "cugan_up2x_denoise3x.onnx"),
}
SHA256 = {
    "RealESRGAN_x4plus_anime_6B.pth": "f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da",
    "2x_APISR_RRDB_GAN_generator.pth": "3b0d2b3a3c0461ac17d00f4f32240666fb832b738ea5a48449b1acf07fbb07e5",
    "up2x-latest-denoise3x.pth": "0a14739f3f5fcbd74ec3ce2806d13a47916c916b20afe4a39d95f6df4ca6abd8",
}


def fetch(key):
    """Return the cached checkpoint path for key, downloading + hash-checking if missing."""
    name, url, _ = MODELS[key]
    path = CACHE / name
    if path.exists():
        return path
    CACHE.mkdir(parents=True, exist_ok=True)
    print(f"downloading {name} ...", flush=True)
    data = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "cui-anime-denoise"})).read()
    if url.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            data = z.read(next(n for n in z.namelist() if n.endswith(name)))
    if hashlib.sha256(data).hexdigest() != SHA256[name]:
        raise SystemExit(f"checksum mismatch for {name} from {url}; not saved")
    path.write_bytes(data)
    return path


def export_one(key):
    import onnx
    import spandrel
    import torch

    name, _, onnx_name = MODELS[key]
    model = spandrel.ModelLoader().load_from_file(str(fetch(key))).model.eval()
    if key == "cugan":
        # UpCunet2x.forward(x, alpha=1) would export a second scalar "alpha"
        # input; bake the default the reference uses instead.
        inner = model

        class _AlphaFixed(torch.nn.Module):
            def __init__(self, m):
                super().__init__()
                self.m = m

            def forward(self, x):
                return self.m(x, 1.0)

        model = _AlphaFixed(inner)
    OUT.mkdir(exist_ok=True)
    dest = OUT / onnx_name

    # dynamo=False: the legacy exporter embeds fp32 weights in a single file and
    # honours opset 17 exactly (the dynamo path emits external .onnx.data and can
    # only downconvert to 17 when a version adapter exists — CUGAN's Pad has none).
    x = torch.zeros(1, 3, 64, 64)
    with torch.no_grad():
        torch.onnx.export(
            model, x, str(dest), dynamo=False,
            opset_version=17,
            input_names=["input"], output_names=["output"],
            dynamic_axes={"input": {0: "N", 2: "H", 3: "W"},
                          "output": {0: "N", 2: "H", 3: "W"}},
        )
    m = onnx.load(str(dest))
    onnx.checker.check_model(m)
    mb = dest.stat().st_size / (1 << 20)
    print(f"{name} -> {dest} ({mb:.1f} MiB, opset {m.opset_import[0].version})", flush=True)
    if key in ("apisr", "cugan"):
        emit_skeleton(key, fetch(key), m, dest.with_suffix(".skel.onnx"))


# ---------------------------------------------------------------------------
# Weightless skeletons (external data -> upstream .pth bytes)
# ---------------------------------------------------------------------------

class _MiniUnpickler:
    """Tiny pickle VM for torch checkpoint payloads (protocol 0-4 subset).

    REDUCE calls are not executed; a ("CALL", funcname, args) node is pushed,
    which is enough to inspect _rebuild_tensor_v2 argument tuples.
    """

    _MARK = object()

    def __init__(self, data):
        self.ops = pickletools.genops(data)
        self.stack = []
        self.memo = {}
        self.pos_of_stop = None

    def run(self):
        for op, arg, pos in self.ops:
            n = op.name
            st = self.stack
            if n == "MARK":
                st.append(self._MARK)
            elif n in ("SHORT_BINUNICODE", "BINUNICODE", "UNICODE", "BINUNICODE8"):
                st.append(arg)
            elif n in ("BININT", "BININT1", "BININT2", "INT", "LONG", "LONG1",
                       "LONG4", "BINFLOAT"):
                st.append(arg)
            elif n in ("BINBYTES", "SHORT_BINBYTES", "BINBYTES8", "BYTEARRAY8"):
                st.append(arg)
            elif n == "NONE":
                st.append(None)
            elif n == "NEWTRUE":
                st.append(True)
            elif n == "NEWFALSE":
                st.append(False)
            elif n == "GLOBAL":
                st.append(("FUNC", arg))
            elif n == "STACK_GLOBAL":
                name = "%s %s" % (st.pop(), st.pop())
                st.append(("FUNC", name))
            elif n in ("BINPERSID", "PERSID"):
                st.append(("PERS", st.pop()))
            elif n in ("TUPLE", "TUPLE1", "TUPLE2", "TUPLE3"):
                items = self._pop_marked() if n == "TUPLE" else \
                    [st.pop() for _ in range(int(n[-1]))][::-1]
                st.append(tuple(items))
            elif n == "EMPTY_TUPLE":
                st.append(())
            elif n == "EMPTY_LIST":
                st.append([])
            elif n == "LIST":
                st.append(self._pop_marked())
            elif n == "APPENDS":
                items = self._pop_marked()
                if isinstance(st[-1], list):
                    st[-1].extend(items)
                else:
                    st[-1] = ("SEQ", st[-1], items)
            elif n == "APPEND":
                v = st.pop()
                if isinstance(st[-1], list):
                    st[-1].append(v)
                else:
                    st[-1] = ("SEQ", st[-1], [v])
            elif n == "EMPTY_DICT":
                st.append({})
            elif n == "DICT":
                items = self._pop_marked()
                st.append(dict(zip(items[::2], items[1::2])))
            elif n == "SETITEM":
                v, k = st.pop(), st.pop()
                self._setitems([k, v])
            elif n == "SETITEMS":
                self._setitems(self._pop_marked())
            elif n in ("BINPUT", "LONG_BINPUT", "PUT"):
                self.memo[arg] = st[-1]
            elif n == "MEMOIZE":
                self.memo[len(self.memo)] = st[-1]
            elif n in ("BINGET", "LONG_BINGET", "GET"):
                st.append(self.memo[arg])
            elif n == "REDUCE":
                args = st.pop()
                fn = st.pop()
                st.append(("CALL", fn, args))
            elif n == "NEWOBJ":
                args = st.pop()
                cls = st.pop()
                st.append(("NEWOBJ", cls, args))
            elif n == "NEWOBJ_EX":
                kw = st.pop()
                args = st.pop()
                cls = st.pop()
                st.append(("NEWOBJ", cls, args))
            elif n == "BUILD":
                state = st.pop()
                obj = st.pop()
                st.append(("BUILD", obj, state))
            elif n == "EMPTY_SET":
                st.append(set())
            elif n == "FROZENSET":
                st.append(frozenset(self._pop_marked()))
            elif n == "ADDITEMS":
                items = self._pop_marked()
                if isinstance(st[-1], set):
                    st[-1].update(items)
                else:
                    st[-1] = ("SEQ", st[-1], items)
            elif n == "POP":
                st.pop()
            elif n == "POP_MARK":
                self._pop_marked()
            elif n == "DUP":
                st.append(st[-1])
            elif n in ("PROTO", "FRAME"):
                pass
            elif n == "STOP":
                self.pos_of_stop = pos
                return st[-1]
            else:
                raise NotImplementedError(n)
        raise ValueError("no STOP")

    def _pop_marked(self):
        items = []
        while self.stack and self.stack[-1] is not self._MARK:
            items.append(self.stack.pop())
        self.stack.pop()  # MARK
        return items[::-1]

    def _setitems(self, items):
        kv = dict(zip(items[::2], items[1::2]))
        target = self.stack[-1]
        if isinstance(target, dict):
            target.update(kv)
        else:
            # a CALL/KV node gaining items (OrderedDict via REDUCE+SETITEMS)
            self.stack[-1] = ("KV", target, kv)


def _rebuild_args(v):
    """(storage_key, storage_offset_elems, size, stride) from a rebuild node."""
    if not (isinstance(v, tuple) and v and isinstance(v[0], str) and
            v[0] in ("CALL", "CALL+STATE")):
        return None
    node = v
    func, args = node[1], node[2]
    fname = func[1] if isinstance(func, tuple) and func[0] == "FUNC" else str(func)
    if "rebuild_tensor" not in fname and "rebuild_parameter" not in fname:
        return None
    storage = args[0]
    if not (isinstance(storage, tuple) and storage and storage[0] == "PERS"):
        return None
    p = storage[1]  # ('storage', dtype, key, location, numel, [view])
    return p[2], args[1], args[2], args[3]


def _param_items(root):
    """Yield (name, value) pairs from any nesting of dicts / KV / BUILD nodes."""
    if isinstance(root, dict):
        for k, v in root.items():
            if _rebuild_args(v) is not None:
                yield k, v
            else:
                yield from _param_items(v)
    elif isinstance(root, (list, set, frozenset)):
        for v in root:
            yield from _param_items(v)
    elif isinstance(root, tuple):
        if root and root[0] == "KV":
            yield from _param_items(root[2])
        else:
            for v in root:
                yield from _param_items(v)


def _pth_ranges(path):
    """Map {param_name: (file_offset, nbytes)} for a torch checkpoint.

    zip container: each storage is a STORED member <arc>/data/<key>; the file
    offset of its bytes is the local header + name/extra lengths.
    legacy stream: after the pickled key list, each storage record is an int64
    element count followed by raw little-endian data.
    """
    if zipfile.is_zipfile(path):
        z = zipfile.ZipFile(path)
        infos = {i.filename: i for i in z.infolist()}
        pkl_name = next(n for n in infos if n.endswith("/data.pkl"))
        data_start = {}
        with open(path, "rb") as f:
            for name, i in infos.items():
                if i.compress_type != zipfile.ZIP_STORED:
                    raise SystemExit(f"{name} is compressed in {path}; "
                                     "external data needs stored bytes")
                f.seek(i.header_offset)
                sig, _, _, _, _, _, _, _, _, nlen, elen = \
                    struct.unpack("<IHHHHHIIIHH", f.read(30))
                if sig != 0x04034B50:
                    raise SystemExit(f"bad local header for {name}")
                data_start[name] = i.header_offset + 30 + nlen + elen
        root = _MiniUnpickler(z.read(pkl_name)).run()
        arc = pkl_name.rsplit("/", 1)[0]
        out = {}
        for name, v in _param_items(root):
            skey, off, size, _stride = _rebuild_args(v)
            numel = 1
            for d in size:
                numel *= d
            out[name] = (data_start[f"{arc}/data/{skey}"] + off * 4, numel * 4)
        return out

    data = path.read_bytes()
    pos = 0
    vals = []
    for _ in range(5):  # magic, version, sys_info, main object, storage keys
        u = _MiniUnpickler(data[pos:])
        vals.append(u.run())
        pos += u.pos_of_stop + 1
    storage_start, keys, main = pos, vals[4], vals[3]
    key_numel = {}

    def scan(o):
        if isinstance(o, tuple):
            if o and o[0] == "PERS" and o[1][0] == "storage":
                key_numel[o[1][2]] = o[1][4]
            for x in o:
                scan(x)
        elif isinstance(o, dict):
            for x in o.values():
                scan(x)
        elif isinstance(o, list):
            for x in o:
                scan(x)
    scan(main)

    ranges = {}
    off = storage_start
    for k in keys:
        (nel,) = struct.unpack("<q", data[off:off + 8])
        if nel != key_numel[k]:
            raise SystemExit(f"storage record for key {k} has {nel} elements, "
                             f"expected {key_numel[k]}")
        ranges[k] = (off + 8, nel * 4)
        off += 8 + nel * 4
    if off != len(data):
        raise SystemExit(f"trailing bytes in {path}: {len(data) - off}")

    out = {}
    for name, v in _param_items(main):
        skey, soff, size, _stride = _rebuild_args(v)
        numel = 1
        for d in size:
            numel *= d
        out[name] = (ranges[skey][0] + soff * 4, numel * 4)
    return out


def emit_skeleton(key, pth_path, model, dest):
    """Rewrite `model`'s initializers as external-data refs into `pth_path`.

    Initializers are matched to checkpoint tensors by content hash, so the
    mapping survives the different parameter names spandrel assigns.
    """
    import onnx

    ranges = _pth_ranges(pth_path)
    raw = pth_path.read_bytes()
    by_hash = {}
    for name, (off, n) in ranges.items():
        by_hash[hashlib.md5(raw[off:off + n]).digest()] = (name, off, n)

    missing = []
    for t in model.graph.initializer:
        import numpy as np
        from onnx import numpy_helper
        arr = numpy_helper.to_array(t)
        if arr.dtype != np.float32 or not arr.flags["C_CONTIGUOUS"]:
            raise SystemExit(f"{key}: initializer {t.name} is not contiguous "
                             f"fp32 (dtype {arr.dtype})")
        h = hashlib.md5(np.ascontiguousarray(arr, "<f4").tobytes()).digest()
        if h not in by_hash:
            missing.append(t.name)
            continue
        _pname, off, n = by_hash[h]
        t.ClearField("raw_data")
        t.data_location = onnx.TensorProto.EXTERNAL
        del t.external_data[:]
        for k, v in (("location", pth_path.name), ("offset", str(off)),
                     ("length", str(n))):
            e = t.external_data.add()
            e.key, e.value = k, v
    if missing:
        raise SystemExit(f"{key}: {len(missing)} initializers have no matching "
                         f"tensor in {pth_path.name}: {missing[:5]}")
    # no onnx.checker here: it resolves the external file eagerly, and the
    # .pth only has to exist next to the skeleton at ORT load time
    dest.write_bytes(model.SerializeToString())
    print(f"  skeleton -> {dest} (weights read from {pth_path.name})", flush=True)


def main():
    keys = sys.argv[1:] or list(MODELS)
    for k in keys:
        if k not in MODELS:
            raise SystemExit(f"unknown model {k!r}; choose from {', '.join(MODELS)}")
        export_one(k)


if __name__ == "__main__":
    main()
