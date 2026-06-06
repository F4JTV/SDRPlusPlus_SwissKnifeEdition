# sdrpp_cw_decoder

Module de décodage CW (Morse) pour **SDR++**, avec décodage adaptatif
inspiré de FLDigi et **réglage de la plage de vitesse**.

L'algorithme de réception (détection d'enveloppe à seuil adaptatif, machine à
états KEYDOWN/KEYUP/QUERY, et suivi de vitesse sur les paires point/trait via un
filtre `Cmovavg`) est porté depuis FLDigi (`src/cw_rtty/cw.cxx`,
`src/cw_rtty/morse.cxx`). Licence : **GPL-3.0** (compatible SDR++ et FLDigi).

## Chaîne de traitement

```
VFO (8 kHz, largeur réglable)
   |
   +-- branche décodage :
   |     -> |z|  (enveloppe, magnitude complexe)   [cw_dsp.h]
   |     -> passe-bas léger sur l'enveloppe
   |     -> décodeur FLDigi @8 kHz, décimation /16  [cw_decoder.h]
   |          - AGC / plancher de bruit adaptatif
   |          - détecteur à hystérésis (CWupper / CWlower)
   |          - mesure des durées d'éléments (smpl_ctr @8 kHz)
   |          - suivi de vitesse borné à [vitesse-plage, vitesse+plage]
   |          - table Morse                          [morse.cpp]
   |     -> texte décodé (UI)
   |
   +-- branche audio :
         -> battement CW (BFO, pitch réglable) + AGC + stéréo
         -> rééchantillonnage vers la fréquence du sink
         -> flux audio enregistré dans le sink (écoute)
```

Le débit interne est figé à 8 kHz afin que les constantes de timing de FLDigi
(`KWPM = 12*8000/10`, `DEC_RATIO = 16`) restent valables sans réajustement.
La sortie du VFO est dédoublée par un `Splitter` : la branche décodage et la
branche audio sont indépendantes (la tonalité n'affecte pas le décodage, qui
travaille sur l'enveloppe avant décalage de fréquence).

## Réglages (menu du module)

- **Vitesse (WPM)** : vitesse nominale, 5 à 60 WPM.
- **Plage (+/- WPM)** : fenêtre de suivi autour de la vitesse nominale. Le
  suivi adaptatif ne sort jamais de `[vitesse-plage, vitesse+plage]`
  (équivalent du `CWrange` de FLDigi, mais réellement contraignant).
- **Suivi auto de vitesse** : active/désactive le tracking. Décoché = vitesse
  fixe.
- **Filtre (Hz)** : largeur du canal VFO (50 à 1000 Hz). ~100-200 Hz convient.
- **Tone (Hz)** : hauteur du battement CW (BFO) en sortie audio, 250 à 1250 Hz.
  N'affecte que l'écoute, pas le décodage.
- **Squelch** : seuil sur la métrique S/N.
- **Afficher les prosignes** : `<BT>`, `<AR>`, `<SK>`, ...

L'audio décodé (battement CW) est envoyé au **sink** sélectionné dans SDR++ :
on peut donc écouter le signal en même temps qu'on le décode, et régler le
volume/la sortie comme pour n'importe quel autre flux.

## Intégration dans l'arbre SDR++

1. Copier ce dossier dans `decoder_modules/cw_decoder` de SDR++ :

   ```bash
   cp -r cw_decoder /chemin/vers/SDRPlusPlus/decoder_modules/
   ```

2. Dans le `CMakeLists.txt` racine de SDR++, ajouter l'option (vers la ligne 55,
   près des autres `OPT_BUILD_*_DECODER`) :

   ```cmake
   option(OPT_BUILD_CW_DECODER "Build the CW (Morse) decoder module (no dependencies required)" ON)
   ```

   puis, dans la section des `add_subdirectory("decoder_modules/...")` :

   ```cmake
   if (OPT_BUILD_CW_DECODER)
   add_subdirectory("decoder_modules/cw_decoder")
   endif (OPT_BUILD_CW_DECODER)
   ```

## Compilation (Ubuntu 24.04)

Dépendances (les mêmes que SDR++ ; aucune dépendance supplémentaire) :

```bash
sudo apt install build-essential cmake libfftw3-dev libglfw3-dev \
     libvolk-dev libzstd-dev libgl1-mesa-dev
```

Build complet :

```bash
cd SDRPlusPlus
mkdir -p build && cd build
cmake .. -DOPT_BUILD_CW_DECODER=ON
make -j$(nproc)
sudo make install   # ou exécuter depuis le dossier build
```

Le module se charge ensuite via *Module Manager* dans SDR++ sous le nom
`cw_decoder`.

## Utilisation

1. Régler le récepteur en mode CW/USB, placer le VFO sur la porteuse Morse.
2. Ajuster **Filtre** sur ~100-200 Hz pour isoler le signal.
3. Régler **Vitesse** au plus près de l'opérateur, **Plage** à 5-10 WPM,
   **Suivi auto** activé : le décodeur se cale tout seul.
   Le premier caractère après l'accord sert à verrouiller l'AGC/le suivi
   (comportement normal de ce type de décodeur).

## Test hors-ligne

`src/test/test_decode.cpp` synthétise l'enveloppe d'un message connu et vérifie
le décodage, indépendamment de SDR++ :

```bash
g++ -std=c++17 -O2 -I src -o test_decode src/test/test_decode.cpp src/morse.cpp
./test_decode 20 "CQ DE F4JTV K"
```
