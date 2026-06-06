#pragma once
//
// APRS symbol icon rendering for the decoded-packets table.
//
// Loads the open-source aprs.fi symbol sheets (hessu/aprs-symbols, CC BY-SA
// 4.0) as OpenGL textures and draws the right cell with ImGui::Image() using
// UV sub-rectangles. The sheets are a 16-column x 8-row grid; the cell index
// of a symbol is (code - '!'), so col = idx % 16, row = idx / 16.
//
// Expected files (copied next to the module's resources, see README):
//     <resourcesDirectory>/aprs/aprs-symbols-64-0.png   primary table  '/'
//     <resourcesDirectory>/aprs/aprs-symbols-64-1.png   secondary '\' + overlay base
//     <resourcesDirectory>/aprs/aprs-symbols-64-2.png   overlay characters
//
// Texture creation must happen on the GUI/OpenGL thread, so loadIfNeeded() is
// called lazily from the draw code (never from the decode thread). If the
// sheets are missing, drawSymbol() returns false and the caller falls back to
// showing the 2-character symbol as text.

#include <string>
#include <filesystem>
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <utils/flog.h>
#include <utils/opengl_include_code.h>

#define STB_IMAGE_STATIC          // keep stb symbols local to this module
#define STB_IMAGE_IMPLEMENTATION
#include <imgui/stb_image.h>

namespace aprs {

    class SymbolSheet {
    public:
        // Number of cells per row/column in the sheet grid. The aprs.fi 64px
        // raster sheets are 1024x384 px = 16 columns x 6 rows of 64px cells.
        static constexpr int COLS = 16;
        static constexpr int ROWS = 6;

        // Draw the symbol (table id + code) at the given pixel size. Returns
        // false if the sheets are unavailable (caller should fall back to text).
        bool drawSymbol(char table, char code, float size) {
            loadIfNeeded();
            if (!loaded) { return false; }

            int idx = (int)(unsigned char)code - 33;   // '!' -> 0
            if (idx < 0 || idx >= COLS * ROWS) { return false; }
            int col = idx % COLS;
            int row = idx / COLS;

            // '/' = primary sheet, anything else uses the secondary sheet (the
            // alternate table '\' and overlay symbols share sheet 1; the overlay
            // glyph itself, if any, is drawn on top from sheet 2).
            bool primary = (table == '/');
            GLuint baseTex = primary ? tex0 : tex1;
            if (baseTex == 0) { return false; }

            ImVec2 uv0((float)col / COLS, (float)row / ROWS);
            ImVec2 uv1((float)(col + 1) / COLS, (float)(row + 1) / ROWS);

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(uintptr_t)baseTex, ImVec2(size, size), uv0, uv1);

            // Overlay character (table is neither '/' nor '\'): draw it on top.
            if (!primary && table != '\\' && tex2 != 0) {
                int oidx = (int)(unsigned char)table - 33;
                if (oidx >= 0 && oidx < COLS * ROWS) {
                    int ocol = oidx % COLS;
                    int orow = oidx / COLS;
                    ImVec2 ouv0((float)ocol / COLS, (float)orow / ROWS);
                    ImVec2 ouv1((float)(ocol + 1) / COLS, (float)(orow + 1) / ROWS);
                    ImGui::GetWindowDrawList()->AddImage(
                        (ImTextureID)(uintptr_t)tex2,
                        cursor, ImVec2(cursor.x + size, cursor.y + size),
                        ouv0, ouv1);
                }
            }
            return true;
        }

        bool available() { loadIfNeeded(); return loaded; }

    private:
        bool   attempted = false;
        bool   loaded = false;
        GLuint tex0 = 0, tex1 = 0, tex2 = 0;

        void loadIfNeeded() {
            if (attempted) { return; }
            attempted = true;   // only try once per session

            std::string resDir;
            try {
                core::configManager.acquire();
                resDir = core::configManager.conf["resourcesDirectory"];
                core::configManager.release();
            } catch (...) {
                resDir = "./res";
            }
            std::string dir = resDir + "/aprs/";

            tex0 = loadTexture(dir + "aprs-symbols-64-0.png");
            tex1 = loadTexture(dir + "aprs-symbols-64-1.png");
            tex2 = loadTexture(dir + "aprs-symbols-64-2.png");

            loaded = (tex0 != 0 && tex1 != 0);
            if (!loaded) {
                flog::warn("[APRS] symbol sheets not found in {0} - showing text symbols. "
                           "See README to install aprs-symbols-64-*.png.", dir);
            }
        }

        static GLuint loadTexture(const std::string& path) {
            if (!std::filesystem::is_regular_file(path)) { return 0; }
            int w, h, n;
            stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 4); // force RGBA
            if (!data) { return 0; }
            GLuint texId = 0;
            glGenTextures(1, &texId);
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t*)data);
            stbi_image_free(data);
            return texId;
        }
    };

}
