/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  KX3Lovers Plus, the built-in theme of the KX3-style display (lcd_panel.c). Included at the end
 *  of theme.c (its tables and statics), and kept out of upstream's lists so their new themes merge
 *  without a conflict: theme.c only calls in here (builtin_index, use_builtin, theme_list), its
 *  index is the one after upstream's last (B_COUNT). Selected by name, as every theme.
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

bool theme_kx3;

/* KX3Lovers: a backlit segment display, dark segments on warm amber, the unlit segments a shade darker
 * than the glass (track). On Outdoor's colours for everything this table does not set */
#define LCD_GLASS 0xe8b04c
#define LCD_INK   0x1e1608
#define LCD_DIM   0x5a4218
#define LCD_OFF   0x9a7430
#define LCD_GHOST 0xd9a240
static const theme_set_t lcd_colors[] = {
    {TC_bg, LCD_GLASS},         {TC_panel, LCD_GLASS},       {TC_bar, LCD_GLASS},           {TC_bar_grad, LCD_GLASS},
    {TC_bar_border, LCD_INK},   {TC_strip, LCD_GLASS},       {TC_strip_grad, LCD_GLASS},    {TC_strip_border, LCD_INK},
    {TC_dialog, LCD_GLASS},     {TC_dialog_grad, LCD_GLASS}, {TC_dialog_border, LCD_INK},   {TC_btn, LCD_GLASS},
    {TC_btn_grad, LCD_GLASS},   {TC_btn_border, LCD_INK},    {TC_btn_text, LCD_INK},        {TC_btn_active, LCD_INK},
    {TC_btn_active_grad, LCD_INK}, {TC_btn_active_border, LCD_INK}, {TC_btn_active_text, LCD_GLASS},
    {TC_btn_page, 0xdca444},    {TC_btn_page_text, LCD_INK}, {TC_btn_off_text, LCD_OFF},    {TC_btn_off_border, LCD_OFF},
    {TC_card, LCD_INK},         {TC_popup, 0xf0c060},        {TC_popup_border, LCD_INK},    {TC_side, LCD_GLASS},
    {TC_chip, 0xdea648},        {TC_sel_bg, LCD_INK},        {TC_sel_text, LCD_GLASS},      {TC_sel_border, LCD_INK},
    {TC_focus, LCD_INK},        {TC_focus_text, LCD_GLASS},  {TC_edited, LCD_DIM},          {TC_vfo, LCD_INK},
    {TC_vfo_bg, 0xd09838},      {TC_text, LCD_INK},          {TC_text_value, LCD_INK},      {TC_text_dim, LCD_DIM},
    {TC_text_soft, LCD_DIM},    {TC_text_mute, LCD_OFF},     {TC_text_faint, 0xc49440},     {TC_text_head, LCD_INK},
    {TC_line, LCD_INK},         {TC_off, LCD_OFF},           {TC_track, LCD_GHOST},         {TC_icon_off, 0xcc9a42},
    {TC_info, LCD_INK},         {TC_accent, LCD_INK},        {TC_ok, LCD_INK},              {TC_toggle_on, LCD_INK},
    {TC_vfo_line, LCD_INK},     {TC_spot, LCD_INK},          {TC_mode_text, LCD_INK},       {TC_mode_bg, LCD_GLASS},
    {TC_mode_border, LCD_INK},  {TC_meter_bar, LCD_INK},     {TC_meter_weak, LCD_INK},      {TC_meter_low, LCD_INK},
    {TC_meter_high, LCD_INK},   {TC_meter_over, LCD_INK},    {TC_pill_off_text, LCD_OFF},   {TC_btn_mark, LCD_OFF},
    {TC_btn_mark_assigned, LCD_DIM}, {TC_btn_mark_active, LCD_INK},
    {TC_list, LCD_GLASS},       {TC_row, 0xdea648},          {TC_frame, LCD_GLASS},         {TC_veil, 0xd09838},
    /* the band plan in shades of the glass, not in colour */
    {TC_bp_cw, 0xc89038},       {TC_bp_dig, 0xd8a044},       {TC_bp_ssb, 0xb88430},         {TC_bp_bcn, LCD_DIM},
    {TC_bp_text, LCD_INK},
};

/* use_builtin(b): true when b is this theme, applied; every other theme turns theme_kx3 off */
bool theme_kx3_use(int b) {
    /* the buttons as text between lines, the active one not inverted */
    static const theme_set_t plus[] = {
        {TC_btn_active, LCD_GLASS}, {TC_btn_active_grad, LCD_GLASS}, {TC_btn_active_text, LCD_INK},
        {TC_btn_page, LCD_GLASS},   {TC_panel, 0xf4c870},   /* the spectrum strip: lighter glass */
    };

    theme_kx3 = b == B_COUNT;
    if (!theme_kx3) return false;
    memcpy(colors, builtin_colors[B_OUTDOOR], sizeof(colors));
    theme_set(lcd_colors, sizeof(lcd_colors) / sizeof(lcd_colors[0]));
    theme_set(plus, sizeof(plus) / sizeof(plus[0]));
    theme_shape = (theme_shape_t){.border = 1, .focus_solid = true, .font = FONT_RCOND, .deck = true,
                                  .mono_cluster = true,
                                  /* the glass to the ink */
                                  .waterfall = {LCD_GLASS, 0xc89038, 0x8a6024, 0x3e2a10, LCD_INK},
                                  .waterfall_n = 5};
    snprintf(theme_shape.name, sizeof(theme_shape.name), "%s", THEME_KX3_NAME);
    return true;
}
