// Auto-generated from dumphfdl etc/systable.conf (version 52).
// Regenerate with tools/gen_systable.py. Do not edit by hand.
#pragma once
#include <cstdint>

struct HfdlStation { int id; double lat; double lon; const char* name; };
struct HfdlFreq    { double khz; int primaryStationId; };

static const int HFDL_SYSTABLE_VERSION = 52;

// Ground stations (id, lat, lon, name).
static const HfdlStation HFDL_STATIONS[] = {
    { 1, 38.384587, -121.759647, "San Francisco, California" },
    { 2, 21.184428, -157.186846, "Molokai, Hawaii" },
    { 3, 63.847168, -22.455754, "Reykjavik, Iceland" },
    { 4, 40.881922, -72.637620, "Riverhead, New York" },
    { 5, -37.015757, 174.809637, "Auckland, New Zealand" },
    { 6, 6.937536, 100.388451, "Hat Yai, Thailand" },
    { 7, 52.744089, -8.926752, "Shannon, Ireland" },
    { 8, -26.129658, 28.206078, "Johannesburg, South Africa" },
    { 9, 71.258490, -156.577447, "Barrow, Alaska" },
    { 10, 35.032377, 126.238644, "Muan, South Korea" },
    { 11, 9.084681, -79.373969, "Albrook, Panama" },
    { 13, -17.671199, -63.157088, "Santa Cruz, Bolivia" },
    { 14, 56.152603, 92.583337, "Krasnoyarsk, Russia" },
    { 15, 26.308529, 50.472318, "Al Muharraq, Bahrain" },
    { 16, 13.488833, 144.828233, "Agana, Guam" },
    { 17, 27.960945, -15.405608, "Canarias, Spain" },
};
static const int HFDL_STATION_CNT = (int)(sizeof(HFDL_STATIONS)/sizeof(HFDL_STATIONS[0]));

// All assigned HFDL channel frequencies (kHz), ascending, with their
// primary (lowest-id) ground station for the menu label.
static const HfdlFreq HFDL_FREQS[] = {
    { 2941.0, 10 },
    { 2944.0, 9 },
    { 2986.0, 15 },
    { 2992.0, 9 },
    { 2998.0, 7 },
    { 3007.0, 9 },
    { 3016.0, 8 },
    { 3455.0, 7 },
    { 3497.0, 9 },
    { 3900.0, 3 },
    { 4654.0, 9 },
    { 4660.0, 13 },
    { 4681.0, 8 },
    { 4687.0, 9 },
    { 5451.0, 16 },
    { 5502.0, 10 },
    { 5508.0, 1 },
    { 5514.0, 2 },
    { 5529.0, 8 },
    { 5538.0, 9 },
    { 5544.0, 9 },
    { 5547.0, 7 },
    { 5583.0, 5 },
    { 5589.0, 11 },
    { 5622.0, 14 },
    { 5652.0, 4 },
    { 5655.0, 6 },
    { 5720.0, 3 },
    { 6529.0, 17 },
    { 6532.0, 7 },
    { 6535.0, 5 },
    { 6559.0, 1 },
    { 6565.0, 2 },
    { 6589.0, 11 },
    { 6596.0, 14 },
    { 6619.0, 10 },
    { 6628.0, 13 },
    { 6646.0, 9 },
    { 6652.0, 16 },
    { 6661.0, 4 },
    { 6712.0, 3 },
    { 8825.0, 6 },
    { 8834.0, 8 },
    { 8843.0, 7 },
    { 8885.0, 15 },
    { 8886.0, 14 },
    { 8894.0, 11 },
    { 8912.0, 2 },
    { 8921.0, 5 },
    { 8927.0, 1 },
    { 8936.0, 2 },
    { 8939.0, 10 },
    { 8942.0, 7 },
    { 8948.0, 17 },
    { 8957.0, 13 },
    { 8977.0, 3 },
    { 10027.0, 2 },
    { 10030.0, 15 },
    { 10060.0, 10 },
    { 10063.0, 11 },
    { 10066.0, 6 },
    { 10081.0, 1 },
    { 10084.0, 5 },
    { 10087.0, 14 },
    { 10093.0, 9 },
    { 11184.0, 3 },
    { 11306.0, 16 },
    { 11312.0, 2 },
    { 11318.0, 13 },
    { 11321.0, 8 },
    { 11327.0, 1 },
    { 11348.0, 2 },
    { 11354.0, 9 },
    { 11384.0, 7 },
    { 11387.0, 4 },
    { 13264.0, 11 },
    { 13270.0, 6 },
    { 13276.0, 1 },
    { 13303.0, 17 },
    { 13312.0, 2 },
    { 13315.0, 13 },
    { 13321.0, 8 },
    { 13324.0, 2 },
    { 13342.0, 10 },
    { 13351.0, 5 },
    { 15025.0, 3 },
    { 17901.0, 11 },
    { 17912.0, 14 },
    { 17916.0, 5 },
    { 17919.0, 1 },
    { 17922.0, 8 },
    { 17928.0, 6 },
    { 17934.0, 9 },
    { 17958.0, 10 },
    { 17967.0, 15 },
    { 17985.0, 3 },
    { 21928.0, 9 },
    { 21931.0, 4 },
    { 21934.0, 1 },
    { 21937.0, 2 },
    { 21949.0, 6 },
    { 21955.0, 17 },
    { 21982.0, 15 },
    { 21990.0, 14 },
    { 21997.0, 13 },
};
static const int HFDL_FREQ_CNT = (int)(sizeof(HFDL_FREQS)/sizeof(HFDL_FREQS[0]));



// Raw systable.conf, written to a temp file at runtime for dumphfdl --system-table.
static const char* HFDL_SYSTABLE_CONF =
    "version = 52;\n"
    "stations = ( \n"
    "  {\n"
    "    id = 1;\n"
    "    lat = 38.384587;\n"
    "    lon = -121.759647;\n"
    "    frequencies = ( 21934.0, 17919.0, 13276.0, 11327.0, 10081.0, 8927.0, 6559.0, 5508.0 );\n"
    "    name = \"San Francisco, California\";\n"
    "  }, \n"
    "  {\n"
    "    id = 2;\n"
    "    lat = 21.184428;\n"
    "    lon = -157.186846;\n"
    "    frequencies = ( 21937.0, 17919.0, 13324.0, 13312.0, 13276.0, 11348.0, 11312.0, 10027.0, 8936.0, 8912.0, 6565.0, 5514.0 );\n"
    "    name = \"Molokai, Hawaii\";\n"
    "  }, \n"
    "  {\n"
    "    id = 3;\n"
    "    lat = 63.847168;\n"
    "    lon = -22.455754;\n"
    "    frequencies = ( 17985.0, 15025.0, 11184.0, 8977.0, 6712.0, 5720.0, 3900.0 );\n"
    "    name = \"Reykjavik, Iceland\";\n"
    "  }, \n"
    "  {\n"
    "    id = 4;\n"
    "    lat = 40.881922;\n"
    "    lon = -72.63762;\n"
    "    frequencies = ( 21931.0, 17919.0, 13276.0, 11387.0, 8912.0, 6661.0, 5652.0 );\n"
    "    name = \"Riverhead, New York\";\n"
    "  }, \n"
    "  {\n"
    "    id = 5;\n"
    "    lat = -37.015757;\n"
    "    lon = 174.809637;\n"
    "    frequencies = ( 17916.0, 13351.0, 10084.0, 8921.0, 6535.0, 5583.0 );\n"
    "    name = \"Auckland, New Zealand\";\n"
    "  }, \n"
    "  {\n"
    "    id = 6;\n"
    "    lat = 6.937536;\n"
    "    lon = 100.388451;\n"
    "    frequencies = ( 21949.0, 17928.0, 13270.0, 10066.0, 8825.0, 6535.0, 5655.0 );\n"
    "    name = \"Hat Yai, Thailand\";\n"
    "  }, \n"
    "  {\n"
    "    id = 7;\n"
    "    lat = 52.744089;\n"
    "    lon = -8.926752;\n"
    "    frequencies = ( 11384.0, 10081.0, 8942.0, 8843.0, 6532.0, 5547.0, 3455.0, 2998.0 );\n"
    "    name = \"Shannon, Ireland\";\n"
    "  }, \n"
    "  {\n"
    "    id = 8;\n"
    "    lat = -26.129658;\n"
    "    lon = 28.206078;\n"
    "    frequencies = ( 21949.0, 17922.0, 13321.0, 11321.0, 8834.0, 5529.0, 4681.0, 3016.0 );\n"
    "    name = \"Johannesburg, South Africa\";\n"
    "  }, \n"
    "  {\n"
    "    id = 9;\n"
    "    lat = 71.25849;\n"
    "    lon = -156.577447;\n"
    "    frequencies = ( 21937.0, 21928.0, 17934.0, 17919.0, 11354.0, 10093.0, 10027.0, 8936.0, 8927.0, 6646.0, 5544.0, 5538.0, 5529.0, 4687.0, 4654.0, 3497.0, 3007.0, 2992.0, 2944.0 );\n"
    "    name = \"Barrow, Alaska\";\n"
    "  }, \n"
    "  {\n"
    "    id = 10;\n"
    "    lat = 35.032377;\n"
    "    lon = 126.238644;\n"
    "    frequencies = ( 21931.0, 17958.0, 13342.0, 10060.0, 8939.0, 6619.0, 5502.0, 2941.0 );\n"
    "    name = \"Muan, South Korea\";\n"
    "  }, \n"
    "  {\n"
    "    id = 11;\n"
    "    lat = 9.084681;\n"
    "    lon = -79.373969;\n"
    "    frequencies = ( 17901.0, 13264.0, 10063.0, 8894.0, 6589.0, 5589.0 );\n"
    "    name = \"Albrook, Panama\";\n"
    "  }, \n"
    "  {\n"
    "    id = 13;\n"
    "    lat = -17.671199;\n"
    "    lon = -63.157088;\n"
    "    frequencies = ( 21997.0, 17916.0, 13315.0, 11318.0, 8957.0, 6628.0, 4660.0 );\n"
    "    name = \"Santa Cruz, Bolivia\";\n"
    "  }, \n"
    "  {\n"
    "    id = 14;\n"
    "    lat = 56.152603;\n"
    "    lon = 92.583337;\n"
    "    frequencies = ( 21990.0, 17912.0, 13321.0, 10087.0, 8886.0, 6596.0, 5622.0 );\n"
    "    name = \"Krasnoyarsk, Russia\";\n"
    "  }, \n"
    "  {\n"
    "    id = 15;\n"
    "    lat = 26.308529;\n"
    "    lon = 50.472318;\n"
    "    frequencies = ( 21982.0, 17967.0, 13312.0, 10030.0, 8885.0, 6646.0, 5544.0, 2986.0 );\n"
    "    name = \"Al Muharraq, Bahrain\";\n"
    "  }, \n"
    "  {\n"
    "    id = 16;\n"
    "    lat = 13.488833;\n"
    "    lon = 144.828233;\n"
    "    frequencies = ( 21928.0, 17919.0, 13312.0, 11306.0, 8927.0, 6652.0, 5451.0 );\n"
    "    name = \"Agana, Guam\";\n"
    "  }, \n"
    "  {\n"
    "    id = 17;\n"
    "    lat = 27.960945;\n"
    "    lon = -15.405608;\n"
    "    frequencies = ( 21955.0, 17928.0, 13303.0, 11348.0, 8948.0, 6529.0 );\n"
    "    name = \"Canarias, Spain\";\n"
    "  } );\n"
;
