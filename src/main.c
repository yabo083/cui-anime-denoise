/* Anime Denoise — hello window.
 *
 * The smallest CUI app: a window, a heading, a line of text and a button.
 * The real UI (image picker, scale/l0 controls, preview, progress) grows here. */

#include <cui/cui.h>
#include <cui/cui_app.h>

int main(void) {
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "Anime Denoise", .width = 520, .height = 340 });
    if (!app) return 1;

    int clicks = 0;
    bool notifications = true;

    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        cui_column(ui, .pad = 32, .gap = 16) {
            cui_heading(ui, "Anime Denoise", 2);
            cui_secondary(ui, "Strip screen-tone moire from GPT anime art. "
                              "CUI front end, Python worker.");
            cui_switch(ui, "Notifications", &notifications);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Click me", .variant = CUI_BUTTON_PRIMARY))
                    clicks++;
                cui_textf(ui, "Clicked %d times", clicks);
            }
        }
    }

    cui_app_destroy(app);
    return 0;
}
