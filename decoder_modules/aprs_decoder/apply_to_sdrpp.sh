#!/usr/bin/env bash
# ============================================================================
#  apply_to_sdrpp.sh — intègre le module aprs_decoder dans l'arbre SDR++.
#
#  Idempotent : vérifie avant chaque insertion, donc relançable sans risque
#  de doublon. À lancer depuis le dossier du module :
#      cd <racine>/decoder_modules/aprs_decoder && ./apply_to_sdrpp.sh
#
#  Patche :
#    - <racine>/CMakeLists.txt   (option + add_subdirectory)
#  N'effectue PAS le patch optionnel de core/src/core.cpp (voir
#  CMAKE_INTEGRATION.txt si vous le souhaitez).
# ============================================================================
set -euo pipefail

# Racine = deux niveaux au-dessus de ce script (.../decoder_modules/aprs_decoder)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CMAKE="$ROOT/CMakeLists.txt"

echo "Racine SDR++   : $ROOT"
echo "CMakeLists.txt : $CMAKE"
echo

if [[ ! -f "$CMAKE" ]]; then
    echo "ERREUR : $CMAKE introuvable. Placez aprs_decoder/ dans <racine>/decoder_modules/." >&2
    exit 1
fi
if [[ ! -d "$ROOT/decoder_modules/aprs_decoder" ]]; then
    echo "ERREUR : <racine>/decoder_modules/aprs_decoder introuvable." >&2
    exit 1
fi

backup="$CMAKE.bak.$(date +%Y%m%d%H%M%S)"
cp "$CMAKE" "$backup"
echo "Sauvegarde : $backup"

# --- PATCH 1 : option(...) -------------------------------------------------
if grep -q "OPT_BUILD_APRS_DECODER" "$CMAKE"; then
    echo "[skip] option(OPT_BUILD_APRS_DECODER ...) déjà présente."
else
    # Insère juste après la ligne de l'option du pager decoder.
    awk '
      /option\(OPT_BUILD_PAGER_DECODER/ && !done {
        print
        print "option(OPT_BUILD_APRS_DECODER \"Build the APRS decoder module (no dependencies required)\" ON)"
        done=1
        next
      }
      { print }
    ' "$CMAKE" > "$CMAKE.tmp" && mv "$CMAKE.tmp" "$CMAKE"
    echo "[ok]   option(OPT_BUILD_APRS_DECODER ...) ajoutée."
fi

# --- PATCH 2 : add_subdirectory(...) ---------------------------------------
if grep -q 'add_subdirectory("decoder_modules/aprs_decoder")' "$CMAKE"; then
    echo "[skip] add_subdirectory(\"decoder_modules/aprs_decoder\") déjà présent."
else
    awk '
      /endif \(OPT_BUILD_PAGER_DECODER\)/ && !done {
        print
        print ""
        print "if (OPT_BUILD_APRS_DECODER)"
        print "add_subdirectory(\"decoder_modules/aprs_decoder\")"
        print "endif (OPT_BUILD_APRS_DECODER)"
        done=1
        next
      }
      { print }
    ' "$CMAKE" > "$CMAKE.tmp" && mv "$CMAKE.tmp" "$CMAKE"
    echo "[ok]   bloc add_subdirectory(aprs_decoder) ajouté."
fi

echo
echo "Vérification des doublons :"
echo "  OPT_BUILD_APRS_DECODER          -> $(grep -c "OPT_BUILD_APRS_DECODER" "$CMAKE")  (attendu : 3 = option + if + endif)"
echo "  decoder_modules/aprs_decoder    -> $(grep -c "decoder_modules/aprs_decoder" "$CMAKE")  (attendu : 1)"
echo
echo "Terminé. Compilez ensuite :"
echo "  cd \"$ROOT\" && mkdir -p build && cd build"
echo "  cmake .. -DOPT_BUILD_APRS_DECODER=ON"
echo "  make -j\$(nproc)"
echo "  sudo make install && sudo ldconfig"
