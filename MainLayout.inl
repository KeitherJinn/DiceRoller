// Included after App/Control/Text/Px; keeps main-window geometry separate from commands.
bool IsFooterControl(int id) {
    return id == IDC_ROLL || id == IDC_CANCEL || id == IDC_SAVE || id == IDC_DISCARD ||
        id == IDC_PROGRESS || id == IDC_STATUS;
}
struct MainPlacement { int id; RECT rect; bool combo = false; };
void Layout(App& app) {
    if (app.layingOut || !Control(app, IDC_STATUS)) return;
    app.layingOut = true;
    struct LayoutGuard { bool& value; ~LayoutGuard() { value = false; } } guard{app.layingOut};
    const int margin = Px(app, 12), gap = Px(app, 8);
    const int line = static_cast<int>(ui::ControlTextSize(Control(app, IDC_ROLL)).cy);
    const int row = (std::max)(Px(app, 28), line + Px(app, 10));
    std::vector<MainPlacement> placements;
    RECT area{};
    auto build = [&] {
        placements.clear();
        app.contentWidth = (std::max)(static_cast<int>(area.right), Px(app, 320));
        const int width = app.contentWidth - 2 * margin;
        // A monitor narrower than 320 DIP uses the existing horizontal fallback;
        // the footer remains vertically fixed while its actions can pan horizontally.
        const int footerWidth = width;
        auto put = [&](int id, int x, int y, int w, int h, bool combo = false) {
            placements.push_back({id, {x, y, x + (std::max)(1, w), y + (std::max)(1, h)}, combo});
        };
        auto textWidth = [&](int id, int padding = 0) {
            return static_cast<int>(ui::ControlTextSize(Control(app, id)).cx) + Px(app, padding);
        };
        auto wrapHeight = [&](int id, int w) { return ui::WrappedTextHeight(Control(app, id), w) + Px(app, 2); };
        const int footerIds[] = {IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD};
        int footerButton = 0;
        for (int id : footerIds) footerButton = (std::max)(footerButton, textWidth(id, 24));
        const int columns = footerWidth >= 4 * footerButton + 3 * gap ? 4 : footerWidth >= 2 * footerButton + gap ? 2 : 1;
        const int actionHeight = (4 / columns) * row + (4 / columns - 1) * gap;
        const int progressHeight = Px(app, 12);
        const int statusMinimum = line + Px(app, 4);
        const int fixedHeight = margin * 2 + actionHeight + gap * 2 + progressHeight;
        const int statusMaximum = (std::max)(statusMinimum,
            (std::min)(line * 3 + Px(app, 4), static_cast<int>(area.bottom) - fixedHeight - Px(app, 48)));
        // The read-only multiline status edit scrolls exceptionally long errors.
        const int statusHeight = (std::min)(statusMaximum, (std::max)(statusMinimum,
            wrapHeight(IDC_STATUS, footerWidth - GetSystemMetricsForDpi(SM_CXVSCROLL, app.dpi) - Px(app, 4)) + Px(app, 4)));
        app.bodyHeight = (std::max)(1, static_cast<int>(area.bottom) - fixedHeight - statusHeight);
        int footerY = app.bodyHeight + margin;
        const int buttonWidth = (footerWidth - (columns - 1) * gap) / columns;
        for (int i = 0; i < 4; ++i)
            put(footerIds[i], margin + (i % columns) * (buttonWidth + gap), footerY + (i / columns) * (row + gap), buttonWidth, row);
        footerY += actionHeight + gap;
        put(IDC_PROGRESS, margin, footerY, footerWidth, progressHeight);
        footerY += progressHeight + gap;
        put(IDC_STATUS, margin, footerY, footerWidth, statusHeight);

        int y = margin;
        int label = Px(app, 62);
        for (int id : {IDC_PRESET_LABEL, IDC_GROUP_LABEL, IDC_QUICK_LABEL, IDC_DICE_LABEL, IDC_NAME_LABEL, IDC_FILE_LABEL})
            label = (std::max)(label, textWidth(id, 4));
        const int apply = textWidth(IDC_PRESET_APPLY, 24), save = textWidth(IDC_PRESET_SAVE, 24), remove = textWidth(IDC_PRESET_DELETE, 24);
        const bool presetWide = width >= label + Px(app, 160) + apply + save + remove + 4 * gap;
        put(IDC_PRESET_LABEL, margin, y + (row - line) / 2, label, line);
        int editWidth = width - label - gap;
        if (presetWide) editWidth -= apply + save + remove + 3 * gap;
        put(IDC_PRESET, margin + label + gap, y, editWidth, row, true);
        int x = margin + label + gap + editWidth + gap;
        if (!presetWide) { y += row + gap; x = margin; }
        put(IDC_PRESET_APPLY, x, y, apply, row); x += apply + gap;
        put(IDC_PRESET_SAVE, x, y, save, row); x += save + gap;
        put(IDC_PRESET_DELETE, x, y, (std::min)(remove, margin + width - x), row);
        y += row + gap;
        const int importWidth = textWidth(IDC_IMPORT, 24), exportWidth = textWidth(IDC_EXPORT_PRESETS, 24);
        const bool groupWide = width >= label + Px(app, 160) + importWidth + exportWidth + 3 * gap;
        put(IDC_GROUP_LABEL, margin, y + (row - line) / 2, label, line);
        editWidth = width - label - gap - (groupWide ? importWidth + exportWidth + 2 * gap : 0);
        put(IDC_GROUP, margin + label + gap, y, editWidth, row, true);
        x = margin + label + gap + editWidth + gap;
        if (!groupWide) { y += row + gap; x = margin; }
        put(IDC_IMPORT, x, y, importWidth, row);
        put(IDC_EXPORT_PRESETS, x + importWidth + gap, y, exportWidth, row);
        y += row + gap;
        const bool narrow = width < Px(app, 540);
        put(IDC_QUICK_LABEL, margin, y + (row - line) / 2, label, line);
        x = margin + label + gap;
        if (narrow) { y += line + gap; x = margin; }
        for (int id = IDC_QUICK_FIRST; id <= IDC_QUICK_LAST; ++id) {
            const int w = (std::max)(Px(app, 40), textWidth(id, 20));
            if (x + w > margin + width) { x = margin; y += row + gap; }
            put(id, x, y, w, row); x += w + gap;
        }
        y += row + gap;
        put(IDC_QUICK_IMMEDIATE, margin, y, width, row);
        y += row + gap;
        put(IDC_DICE_LABEL, margin, y + (row - line) / 2, label, line);
        const int timesLabel = textWidth(IDC_TIMES_LABEL, 4), timesEdit = Px(app, 90);
        put(IDC_DICE_EDIT, margin + label + gap, y,
            width - label - gap - (narrow ? 0 : timesLabel + timesEdit + 3 * gap), row);
        if (narrow) { y += row + gap; x = margin; }
        else x = margin + width - timesLabel - timesEdit - gap;
        put(IDC_TIMES_LABEL, x, y + (row - line) / 2, timesLabel, line);
        put(IDC_TIMES_EDIT, x + timesLabel + gap, y, timesEdit, row);
        y += row + gap;
        const int normalWidth = textWidth(IDC_RAD_NORMAL, 26), advantageWidth = textWidth(IDC_RAD_ADV, 26), disadvantageWidth = textWidth(IDC_RAD_DISADV, 26);
        const bool stackModes = normalWidth + advantageWidth + disadvantageWidth + 4 * gap > width;
        const int modeHeight = line + gap + (stackModes ? 3 * row + gap * 2 : row) + gap;
        put(IDC_MODE_GROUP, margin, y, width, modeHeight);
        x = margin + gap; int modeY = y + line + gap;
        put(IDC_RAD_NORMAL, x, modeY, normalWidth, row);
        if (stackModes) modeY += row + gap; else x += normalWidth + gap;
        put(IDC_RAD_ADV, x, modeY, advantageWidth, row);
        if (stackModes) modeY += row + gap; else x += advantageWidth + gap;
        put(IDC_RAD_DISADV, x, modeY, disadvantageWidth, row);
        y += modeHeight + gap;
        const int rulesHeight = wrapHeight(IDC_RULES, width);
        put(IDC_RULES, margin, y, width, rulesHeight); y += rulesHeight + gap;
        put(IDC_NAME_LABEL, margin, y + (row - line) / 2, label, line);
        put(IDC_NAME_EDIT, margin + label + gap, y, width - label - gap, row); y += row + gap;
        const int targetWidth = textWidth(IDC_TARGET, 26), compareWidth = Px(app, 64);
        const int targetValueWidth = narrow ? width - targetWidth - compareWidth - 2 * gap : Px(app, 150);
        put(IDC_TARGET, margin, y, targetWidth, row);
        put(IDC_COMPARISON, margin + targetWidth + gap, y, compareWidth, row, true);
        put(IDC_TARGET_VALUE, margin + targetWidth + compareWidth + 2 * gap, y, targetValueWidth, row);
        const int poolWidth = textWidth(IDC_POOL, 26);
        x = margin + targetWidth + compareWidth + targetValueWidth + 3 * gap;
        if (narrow || x + poolWidth + gap + Px(app, 70) > margin + width) { y += row + gap; x = margin; }
        put(IDC_POOL, x, y, poolWidth, row);
        put(IDC_POOL_VALUE, x + poolWidth + gap, y, margin + width - x - poolWidth - gap, row); y += row + gap;
        const int timestampWidth = textWidth(IDC_TIMESTAMP, 26), autosaveWidth = textWidth(IDC_AUTOSAVE, 26);
        put(IDC_TIMESTAMP, margin, y, timestampWidth, row);
        x = margin + timestampWidth + gap;
        if (timestampWidth + autosaveWidth + gap > width) { y += row + gap; x = margin; }
        put(IDC_AUTOSAVE, x, y, autosaveWidth, row); y += row + gap;
        const int browseWidth = textWidth(IDC_BROWSE, 24);
        put(IDC_FILE_LABEL, margin, y + (narrow ? 0 : (row - line) / 2), label, line);
        x = margin + label + gap;
        if (narrow) { y += line + gap; x = margin; }
        put(IDC_FILE_EDIT, x, y, margin + width - x - browseWidth - gap, row);
        put(IDC_BROWSE, margin + width - browseWidth, y, browseWidth, row); y += row + gap;
        x = margin;
        for (int id : {IDC_PROBABILITY, IDC_OPEN, IDC_COPY, IDC_CLEAR}) {
            const int w = textWidth(id, 24);
            if (x + w > margin + width) { x = margin; y += row + gap; }
            put(id, x, y, w, row); x += w + gap;
        }
        y += row + gap;
        const int outputLabelHeight = wrapHeight(IDC_OUTPUT_LABEL, width);
        put(IDC_OUTPUT_LABEL, margin, y, width, outputLabelHeight); y += outputLabelHeight + gap;
        const int retentionHeight = wrapHeight(IDC_RETENTION, width);
        const int outputHeight = (std::max)(Px(app, 140), app.bodyHeight - y - retentionHeight - gap - margin);
        put(IDC_OUTPUT, margin, y, width, outputHeight); y += outputHeight + gap;
        put(IDC_RETENTION, margin, y, width, retentionHeight); y += retentionHeight + margin;
        app.contentHeight = (std::max)(y, app.bodyHeight);
    };
    for (int pass = 0; pass < 4; ++pass) {
        GetClientRect(app.window, &area); build();
        app.scrollX = (std::max)(0, (std::min)(app.scrollX, app.contentWidth - static_cast<int>(area.right)));
        app.scrollY = (std::max)(0, (std::min)(app.scrollY, app.contentHeight - app.bodyHeight));
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0,
            app.contentWidth - 1, static_cast<UINT>(area.right), app.scrollX, 0};
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0,
            app.contentHeight - 1, static_cast<UINT>(app.bodyHeight), app.scrollY, 0};
        SetScrollInfo(app.window, SB_HORZ, &horizontal, TRUE);
        SetScrollInfo(app.window, SB_VERT, &vertical, TRUE);
        RECT next{}; GetClientRect(app.window, &next);
        if (EqualRect(&area, &next)) break;
    }
    // A real parent viewport clips every child's client and nonclient painting
    // at one shared boundary above the fixed actions.
    MoveWindow(app.body, 0, 0, area.right, app.bodyHeight, FALSE);
    for (const auto& placement : placements) {
        const bool footer = IsFooterControl(placement.id);
        const auto child = Control(app, placement.id);
        const auto& rect = placement.rect;
        MoveWindow(child, rect.left - app.scrollX, rect.top - (footer ? 0 : app.scrollY),
            rect.right - rect.left, placement.combo ? Px(app, 240) : rect.bottom - rect.top, FALSE);
        if (footer && app.contentWidth > area.right) {
            const RECT footerViewport{0, app.bodyHeight, area.right, area.bottom};
            ui::ClipChildToViewport(child, app.window, footerViewport);
        }
        else if (footer) SetWindowRgn(child, nullptr, FALSE);
    }
    ui::RedrawLayout(app.window);
}

void EnsureFocusVisible(App& app) {
    auto focus = GetFocus();
    if (!focus || !IsChild(app.window, focus)) return;
    // Editable comboboxes contain an edit child; scroll the complete combobox.
    while (GetParent(focus) != app.window && GetParent(focus) != app.body &&
        IsChild(app.window, GetParent(focus))) focus = GetParent(focus);
    if (focus == app.body) return;
    const bool footer = IsFooterControl(GetDlgCtrlID(focus));
    RECT bounds{}, client{};
    GetWindowRect(focus, &bounds);
    MapWindowPoints(nullptr, app.window, reinterpret_cast<POINT*>(&bounds), 2);
    GetClientRect(app.window, &client); client.bottom = app.bodyHeight;
    const int oldX = app.scrollX, oldY = app.scrollY;
    if (!footer) {
        if (bounds.top < 0) app.scrollY += bounds.top;
        else if (bounds.bottom > client.bottom) app.scrollY += (std::min)(bounds.top, bounds.bottom - client.bottom);
    }
    if (bounds.left < 0) app.scrollX += bounds.left;
    else if (bounds.right > client.right) app.scrollX += (std::min)(bounds.left, bounds.right - client.right);
    if (oldX != app.scrollX || oldY != app.scrollY) Layout(app);
}
