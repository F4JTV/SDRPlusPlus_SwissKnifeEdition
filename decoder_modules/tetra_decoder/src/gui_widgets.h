#pragma once
#define GImGui (ImGui::GetCurrentContext())

#include <imgui/imgui.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ImGui {

    // ---------- Math helpers ----------------------------------------------------
    inline ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
    inline ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }

    // ---------- Inline coloured square indicator --------------------------------
    // Drawn at the current cursor position (proper inline behaviour).
    // `size` is the side length in pixels. If <=0, defaults to the font size.
    inline void BoxIndicator(float size, ImU32 color) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        if (size <= 0.0f) size = GetFontSize();

        ImVec2 p = window->DC.CursorPos;
        ImVec2 sz = ImVec2(size, size);
        ImRect bb(p, p + sz);

        ItemSize(sz, GetStyle().FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        // Subtle border + filled core for a cleaner look on dark themes.
        window->DrawList->AddRectFilled(p, p + sz, color, 2.0f);
        window->DrawList->AddRect(p, p + sz, IM_COL32(0, 0, 0, 90), 2.0f);
    }

    // ---------- LED-style round indicator ---------------------------------------
    inline void StatusLed(bool active, float size = 0.0f,
                          ImU32 on_color  = IM_COL32(  5, 230,   5, 255),
                          ImU32 off_color = IM_COL32(230,   5,   5, 255)) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        if (size <= 0.0f) size = GetFontSize();
        ImVec2 p  = window->DC.CursorPos;
        ImVec2 sz = ImVec2(size, size);
        ImRect bb(p, p + sz);

        ItemSize(sz, GetStyle().FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        ImVec2 center = ImVec2(p.x + size * 0.5f, p.y + size * 0.5f);
        float  radius = size * 0.42f;

        ImU32 col = active ? on_color : off_color;
        // Outer halo (faint) + main disc + small specular highlight.
        window->DrawList->AddCircleFilled(center, radius * 1.25f,
                                          (col & 0x00FFFFFF) | 0x30000000, 16);
        window->DrawList->AddCircleFilled(center, radius, col, 16);
        window->DrawList->AddCircle(center, radius, IM_COL32(0, 0, 0, 120), 16, 1.0f);
        window->DrawList->AddCircleFilled(
            ImVec2(center.x - radius * 0.3f, center.y - radius * 0.3f),
            radius * 0.25f, IM_COL32(255, 255, 255, 110), 8);
    }

    // ---------- Signal-quality meter with numeric overlay -----------------------
    inline void SigQualityMeter(float avg, float val_min, float val_max,
                                const ImVec2& size_arg = ImVec2(0, 0),
                                bool drawText = true) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        ImGuiStyle& style = GetStyle();
        avg = std::clamp<float>(avg, val_min, val_max);

        ImVec2 p  = window->DC.CursorPos;
        ImVec2 sz = CalcItemSize(size_arg, CalcItemWidth(),
                                 GetFontSize() + style.FramePadding.y * 2);
        ImRect bb(p, p + sz);

        ItemSize(sz, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        // Background track + red "bad" / green "good" zones.
        float threshold = roundf(0.3f * sz.x);
        window->DrawList->AddRectFilled(p,                        p + ImVec2(threshold, sz.y), IM_COL32(60, 18, 18, 255), 3.0f);
        window->DrawList->AddRectFilled(p + ImVec2(threshold, 0), p + sz,                       IM_COL32(18, 60, 18, 255), 3.0f);

        // Filled portion based on current value.
        float end = roundf(((avg - val_min) / (val_max - val_min)) * sz.x);
        if (avg <= val_min + (val_max - val_min) * 0.3f) {
            window->DrawList->AddRectFilled(p, p + ImVec2(end, sz.y), IM_COL32(230, 60, 60, 255), 3.0f);
        } else {
            window->DrawList->AddRectFilled(p,                        p + ImVec2(threshold, sz.y), IM_COL32(230, 60, 60, 255), 3.0f);
            window->DrawList->AddRectFilled(p + ImVec2(threshold, 0), p + ImVec2(end,        sz.y), IM_COL32( 60, 200, 60, 255), 3.0f);
        }

        // Border + threshold tick.
        window->DrawList->AddRect(p, p + sz, IM_COL32(0, 0, 0, 140), 3.0f);
        window->DrawList->AddLine(p + ImVec2(threshold, 0), p + ImVec2(threshold, sz.y),
                                  IM_COL32(255, 255, 255, 80), 1.0f);

        if (drawText) {
            char buf[16];
            float pct = (avg - val_min) / (val_max - val_min) * 100.0f;
            std::snprintf(buf, sizeof(buf), "%.0f %%", pct);
            ImVec2 ts = CalcTextSize(buf);
            ImVec2 tp = ImVec2(p.x + (sz.x - ts.x) * 0.5f,
                               p.y + (sz.y - ts.y) * 0.5f);
            // Subtle text shadow for legibility on either coloured side.
            window->DrawList->AddText(tp + ImVec2(1, 1), IM_COL32(0, 0, 0, 200), buf);
            window->DrawList->AddText(tp,                IM_COL32(255, 255, 255, 230), buf);
        }
    }

    // ---------- Pill / status badge ---------------------------------------------
    // A rounded, padded badge that turns green/red (or custom) depending on state.
    inline void StatusPill(const char* label, bool active,
                           ImU32 on_color  = IM_COL32( 30, 140,  60, 255),
                           ImU32 off_color = IM_COL32( 90,  35,  35, 220),
                           ImU32 text_on   = IM_COL32(235, 255, 235, 255),
                           ImU32 text_off  = IM_COL32(200, 200, 200, 200)) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        ImGuiStyle& style = GetStyle();
        ImVec2 ts   = CalcTextSize(label);
        ImVec2 pad  = ImVec2(8.0f, 3.0f);
        ImVec2 sz   = ImVec2(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
        ImVec2 p    = window->DC.CursorPos;
        ImRect bb(p, p + sz);

        ItemSize(sz, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        ImU32 bg   = active ? on_color : off_color;
        ImU32 fg   = active ? text_on  : text_off;
        float rnd  = sz.y * 0.5f;

        window->DrawList->AddRectFilled(p, p + sz, bg, rnd);
        window->DrawList->AddRect(p, p + sz,
                                  active ? IM_COL32(255, 255, 255, 50)
                                         : IM_COL32(255, 255, 255, 30),
                                  rnd, 0, 1.0f);
        window->DrawList->AddText(p + pad, fg, label);
    }

    // ---------- Single timeslot cell --------------------------------------------
    // Used by TimeslotIndicator below. Each cell shows the slot number on top and
    // its content tag below (UL / DATA / NDB / SYNC / VOICE).
    // `encMode`: 0 = clear, 1 = SCK, 2 = CCK, 3 = CCK + AI mode 3. When >0, a
    //            small padlock badge (E1/E2/E3) is overlaid in the top-right.
    inline void TimeslotCell(int slotIdx, int content, int encMode,
                             float cellWidth, float cellHeight) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        const char* label;
        ImU32       bg, fg;
        switch (content) {
            case 1: label = "DATA";  bg = IM_COL32(140, 30, 140, 230); fg = IM_COL32(255, 230, 255, 255); break;
            case 2: label = "NDB";   bg = IM_COL32(140, 130, 30, 230); fg = IM_COL32(255, 250, 200, 255); break;
            case 3: label = "SYNC";  bg = IM_COL32(20, 120, 140, 230); fg = IM_COL32(200, 245, 255, 255); break;
            case 4: label = "VOICE"; bg = IM_COL32(20, 130, 30, 240);  fg = IM_COL32(220, 255, 220, 255); break;
            default: label = "UL";   bg = IM_COL32(55, 55, 55, 200);   fg = IM_COL32(190, 190, 190, 255); break;
        }

        ImVec2 p  = window->DC.CursorPos;
        ImVec2 sz = ImVec2(cellWidth, cellHeight);
        ImRect bb(p, p + sz);

        ItemSize(sz, GetStyle().FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        // Outer rounded background, then a coloured body band and a darker
        // header strip drawn on top. No corner-flag dependency this way.
        window->DrawList->AddRectFilled(p, p + sz, bg, 3.0f);
        float headerH = cellHeight * 0.40f;
        window->DrawList->AddRectFilled(p, p + ImVec2(sz.x, headerH),
                                        IM_COL32(35, 35, 35, 230), 3.0f);
        // Cover the bottom of the header rectangle so its rounding doesn't
        // leak into the body half.
        window->DrawList->AddRectFilled(p + ImVec2(0, headerH * 0.5f),
                                        p + ImVec2(sz.x, headerH),
                                        IM_COL32(35, 35, 35, 230));
        window->DrawList->AddRect(p, p + sz, IM_COL32(0, 0, 0, 140), 3.0f);

        char slotBuf[16];
        std::snprintf(slotBuf, sizeof(slotBuf), "TS%d", slotIdx + 1);
        ImVec2 ts1 = CalcTextSize(slotBuf);
        window->DrawList->AddText(
            ImVec2(p.x + (sz.x - ts1.x) * 0.5f, p.y + (headerH - ts1.y) * 0.5f),
            IM_COL32(220, 220, 220, 255), slotBuf);

        ImVec2 ts2 = CalcTextSize(label);
        window->DrawList->AddText(
            ImVec2(p.x + (sz.x - ts2.x) * 0.5f,
                   p.y + headerH + ((sz.y - headerH) - ts2.y) * 0.5f),
            fg, label);

        // Encryption badge: small "E<mode>" in the top-right of the header,
        // amber background. Only drawn when actually observed (encMode > 0).
        if (encMode > 0 && encMode <= 3) {
            char ebuf[4];
            std::snprintf(ebuf, sizeof(ebuf), "E%d", encMode);
            ImVec2 etxt = CalcTextSize(ebuf);
            ImVec2 pad  = ImVec2(3.0f, 1.0f);
            ImVec2 ebbSize = ImVec2(etxt.x + pad.x * 2, etxt.y + pad.y * 2);
            ImVec2 ep      = ImVec2(p.x + sz.x - ebbSize.x - 2.0f, p.y + 2.0f);
            window->DrawList->AddRectFilled(ep, ep + ebbSize,
                                            IM_COL32(170, 110, 30, 230),
                                            ebbSize.y * 0.5f);
            window->DrawList->AddText(ep + pad,
                                      IM_COL32(255, 245, 220, 255), ebuf);
        }
    }

    // ---------- Full timeslot row (TS1..TS4) ------------------------------------
    // `encModes` may be NULL; otherwise it must point to 4 entries with the
    // observed encryption mode for each slot (0 = clear).
    inline void TimeslotIndicator(const int contents[4], const int encModes[4],
                                  float totalWidth) {
        ImGuiStyle& style = GetStyle();
        float spacing = style.ItemSpacing.x;
        float cellW   = (totalWidth - spacing * 3.0f) / 4.0f;
        float cellH   = GetFontSize() * 2.4f;

        for (int i = 0; i < 4; i++) {
            int em = encModes ? encModes[i] : 0;
            TimeslotCell(i, contents[i], em, cellW, cellH);
            if (i < 3) SameLine();
        }
    }

    // ---------- Per-slot caller info cell ---------------------------------------
    // Shows "TS N" header + SSI value + colored dot indicating call state.
    // ssi == 0 means "no traffic observed yet on this slot", rendered greyed.
    // ageMs is "milliseconds since last activity"; cells fade after 5s.
    inline void SlotInfoCell(int slotIdx, int ssi, int callState,
                             uint64_t ageMs,
                             float cellWidth, float cellHeight) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        ImVec2 p  = window->DC.CursorPos;
        ImVec2 sz = ImVec2(cellWidth, cellHeight);
        ImRect bb(p, p + sz);
        ItemSize(sz, GetStyle().FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        bool active = (ssi != 0) && (ageMs < 5000);
        bool stale  = (ssi != 0) && (ageMs >= 5000);

        ImU32 bg = active ? IM_COL32(30, 65, 30, 230)
                          : (stale ? IM_COL32(55, 50, 30, 200)
                                   : IM_COL32(40, 40, 40, 180));
        window->DrawList->AddRectFilled(p, p + sz, bg, 3.0f);
        window->DrawList->AddRect(p, p + sz, IM_COL32(0, 0, 0, 140), 3.0f);

        // Header strip
        float headerH = cellHeight * 0.32f;
        window->DrawList->AddRectFilled(p, p + ImVec2(sz.x, headerH),
                                        IM_COL32(28, 28, 28, 230), 3.0f);
        window->DrawList->AddRectFilled(p + ImVec2(0, headerH * 0.5f),
                                        p + ImVec2(sz.x, headerH),
                                        IM_COL32(28, 28, 28, 230));

        char slotBuf[16];
        std::snprintf(slotBuf, sizeof(slotBuf), "TS%d", slotIdx + 1);
        ImVec2 ts1 = CalcTextSize(slotBuf);
        window->DrawList->AddText(
            ImVec2(p.x + 4.0f, p.y + (headerH - ts1.y) * 0.5f),
            IM_COL32(220, 220, 220, 255), slotBuf);

        // State dot in the top-right of the header
        float dotR = headerH * 0.20f;
        ImVec2 dotC = ImVec2(p.x + sz.x - dotR * 2.0f - 4.0f,
                             p.y + headerH * 0.5f);
        ImU32 dotCol;
        switch (callState) {
            case 1:  dotCol = IM_COL32(120, 220, 120, 255); break; // traffic
            case 2:  dotCol = IM_COL32(230, 200,  80, 255); break; // ringing
            case 3:  dotCol = IM_COL32(120, 220, 120, 255); break; // connected
            default: dotCol = IM_COL32(120, 120, 120, 200); break; // idle
        }
        if (!active) dotCol = (dotCol & 0x00FFFFFF) | 0x60000000;
        window->DrawList->AddCircleFilled(dotC, dotR, dotCol, 10);

        // Body: SSI value or "—"
        char ssiBuf[20];
        if (ssi) std::snprintf(ssiBuf, sizeof(ssiBuf), "%d", ssi);
        else     std::snprintf(ssiBuf, sizeof(ssiBuf), "—");

        ImVec2 ts2 = CalcTextSize(ssiBuf);
        ImU32 ssiCol = active ? IM_COL32(245, 245, 245, 255)
                              : IM_COL32(180, 180, 180, 200);
        window->DrawList->AddText(
            ImVec2(p.x + (sz.x - ts2.x) * 0.5f,
                   p.y + headerH + ((sz.y - headerH) - ts2.y) * 0.5f),
            ssiCol, ssiBuf);
    }

    inline void SlotInfoRow(const int ssis[4], const int callStates[4],
                            const uint64_t ageMs[4], float totalWidth) {
        ImGuiStyle& style = GetStyle();
        float spacing = style.ItemSpacing.x;
        float cellW   = (totalWidth - spacing * 3.0f) / 4.0f;
        float cellH   = GetFontSize() * 2.8f;
        for (int i = 0; i < 4; i++) {
            SlotInfoCell(i, ssis[i], callStates[i], ageMs[i], cellW, cellH);
            if (i < 3) SameLine();
        }
    }

    // ---------- Labeled value (e.g. "MCC: 208") ---------------------------------
    inline void LabeledValue(const char* label, const char* value,
                             ImU32 valueColor = IM_COL32(240, 230, 140, 255)) {
        TextUnformatted(label);
        SameLine(0.0f, 4.0f);
        PushStyleColor(ImGuiCol_Text, valueColor);
        TextUnformatted(value);
        PopStyleColor();
    }

    // ---------- Hyperframe/Multiframe/Frame counters ---------------------------
    // Three big monospace counters separated by thin vertical bars.
    inline void FrameCounterDisplay(int hyperframe, int multiframe, int frame,
                                    float totalWidth) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return;

        ImGuiStyle& style = GetStyle();
        float h = GetFontSize() * 2.6f;
        ImVec2 p  = window->DC.CursorPos;
        ImVec2 sz = ImVec2(totalWidth, h);

        ImRect bb(p, p + sz);
        ItemSize(sz, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) return;

        window->DrawList->AddRectFilled(p, p + sz, IM_COL32(30, 30, 35, 220), 4.0f);
        window->DrawList->AddRect(p, p + sz, IM_COL32(0, 0, 0, 150), 4.0f);

        struct Entry { const char* lbl; char val[16]; };
        Entry entries[3];
        entries[0].lbl = "HYPERFRAME"; std::snprintf(entries[0].val, 16, "%05d", hyperframe);
        entries[1].lbl = "MULTIFRAME"; std::snprintf(entries[1].val, 16, "%02d",  multiframe);
        entries[2].lbl = "FRAME";      std::snprintf(entries[2].val, 16, "%02d",  frame);

        float colW = sz.x / 3.0f;
        for (int i = 0; i < 3; i++) {
            ImVec2 colTL = ImVec2(p.x + colW * i, p.y);

            ImVec2 lblSize = CalcTextSize(entries[i].lbl);
            window->DrawList->AddText(
                ImVec2(colTL.x + (colW - lblSize.x) * 0.5f, colTL.y + 4.0f),
                IM_COL32(170, 170, 170, 230), entries[i].lbl);

            ImVec2 valSize = CalcTextSize(entries[i].val);
            window->DrawList->AddText(
                ImVec2(colTL.x + (colW - valSize.x * 1.4f) * 0.5f,
                       colTL.y + h - valSize.y - 4.0f),
                IM_COL32(245, 230, 100, 255), entries[i].val);

            if (i < 2) {
                window->DrawList->AddLine(
                    ImVec2(colTL.x + colW, colTL.y + 6.0f),
                    ImVec2(colTL.x + colW, colTL.y + h - 6.0f),
                    IM_COL32(255, 255, 255, 40), 1.0f);
            }
        }
    }

    // ---------- Frequency row with usage tag -----------------------------------
    inline void FreqRow(const char* dirLabel, int freqHz, const char* usageStr,
                        ImU32 dirColor = IM_COL32(180, 200, 255, 255)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%9.4f MHz", freqHz / 1000000.0f);

        PushStyleColor(ImGuiCol_Text, dirColor);
        TextUnformatted(dirLabel);
        PopStyleColor();
        SameLine();
        PushStyleColor(ImGuiCol_Text, IM_COL32(245, 230, 100, 255));
        TextUnformatted(buf);
        PopStyleColor();
        SameLine();
        StatusPill(usageStr, true,
                   IM_COL32(60, 90, 130, 220),
                   IM_COL32(60, 90, 130, 220));
    }

} // namespace ImGui
