# Compilation du module `aprs_decoder` (in-tree, Ubuntu 24.04)

Ce module se compile **dans l'arborescence de SDR++** (méthode *in-tree*), comme
les modules officiels (`pager_decoder`, `meteor_demodulator`, …) et comme le
module ACARS. Il n'y a pas de build séparé : le module est construit en même
temps que SDR++ via son mécanisme CMake interne (`sdrpp_module.cmake`).

Cible : **Ubuntu 24.04 LTS**.

---

## 1. Prérequis

- Ubuntu 24.04 LTS
- CMake ≥ 3.13
- Compilateur C++17 (GCC 13 d'Ubuntu 24 convient)

Le module **n'ajoute aucune dépendance** : il n'utilise que le cœur de SDR++
(`sdrpp_core`). Les seules dépendances sont donc celles de SDR++ lui-même.

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libfftw3-dev \
    libglfw3-dev \
    libvolk-dev \
    libzstd-dev \
    librtlsdr-dev
```

> `librtlsdr-dev` couvre la RTL-SDR. Ajoutez au besoin les paquets `*-dev` des
> autres clés (`libhackrf-dev`, `libairspy-dev`, `libairspyhf-dev`,
> `libsoapysdr-dev`, …) selon votre matériel.

---

## 2. Récupérer SDR++ et placer le module

```bash
cd ~
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus

# Copier le module dans decoder_modules/
cp -r /chemin/vers/aprs_decoder decoder_modules/
```

Après cette étape on doit avoir :

```
SDRPlusPlus/
└── decoder_modules/
    └── aprs_decoder/
        ├── CMakeLists.txt
        └── src/…
```

Le `CMakeLists.txt` du module est minimal (in-tree) — toute la logique de link
sur `sdrpp_core`, d'includes et d'installation est gérée par
`${SDRPP_MODULE_CMAKE}` :

```cmake
cmake_minimum_required(VERSION 3.13)
project(aprs_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(aprs_decoder PRIVATE "src/")
```

---

## 3. Brancher le module dans le `CMakeLists.txt` racine de SDR++

Deux possibilités.

### 3.a — Automatique (recommandé)

Le script fourni applique les deux patchs CMake, de façon **idempotente** (on
peut le relancer sans créer de doublon) :

```bash
cd ~/SDRPlusPlus/decoder_modules/aprs_decoder
./apply_to_sdrpp.sh
cd ~/SDRPlusPlus
```

### 3.b — Manuelle

Éditer `~/SDRPlusPlus/CMakeLists.txt` et ajouter **deux** blocs.

**1) L'option de compilation** (section `# Decoders`, vers la ligne 55).
Repérer :

```cmake
option(OPT_BUILD_METEOR_DEMODULATOR "Build the meteor demodulator module (no dependencies required)" ON)
option(OPT_BUILD_PAGER_DECODER "Build the pager decoder module (no dependencies required)" ON)
option(OPT_BUILD_RADIO "Main audio modulation decoder (AM, FM, SSB, etc...)" ON)
```

…et ajouter, juste après la ligne `PAGER_DECODER` :

```cmake
option(OPT_BUILD_APRS_DECODER "Build the APRS decoder module (no dependencies required)" ON)
```

**2) Le sous-répertoire** (seconde section `# Decoders`, vers la ligne 291).
Repérer :

```cmake
if (OPT_BUILD_PAGER_DECODER)
add_subdirectory("decoder_modules/pager_decoder")
endif (OPT_BUILD_PAGER_DECODER)
```

…et ajouter, juste après ce bloc :

```cmake
if (OPT_BUILD_APRS_DECODER)
add_subdirectory("decoder_modules/aprs_decoder")
endif (OPT_BUILD_APRS_DECODER)
```

#### Récapitulatif

| Emplacement | Ligne(s) à ajouter |
|---|---|
| Section `option(...)` (~ligne 55) | `option(OPT_BUILD_APRS_DECODER "Build the APRS decoder module (no dependencies required)" ON)` |
| Section `add_subdirectory(...)` (~ligne 291) | `if (OPT_BUILD_APRS_DECODER)`<br>`add_subdirectory("decoder_modules/aprs_decoder")`<br>`endif (OPT_BUILD_APRS_DECODER)` |

---

## 4. Compiler et installer

```bash
cd ~/SDRPlusPlus
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_APRS_DECODER=ON
make -j$(nproc)
sudo make install
sudo ldconfig
```

> **Compiler uniquement le module** (après un premier build complet de SDR++) :
> ```bash
> cd ~/SDRPlusPlus/build
> make aprs_decoder -j$(nproc)
> sudo make install && sudo ldconfig
> ```

---

## 5. Activer le module dans SDR++

1. Lancer `sdrpp`.
2. Ouvrir le **Module Manager** (menu de gauche).
3. *Type* = `aprs_decoder`, *Name* = par ex. `APRS`, puis cliquer **+**.
4. Le module « APRS » apparaît dans le panneau de gauche.

> Pour décoder deux fréquences en parallèle, ajouter **deux instances** (le
> nombre d'instances n'est pas limité).

---

## 6. Fichiers produits

```
/usr/lib/sdrpp/plugins/
└── aprs_decoder.so            # le module compilé

~/.config/sdrpp/
└── aprs_decoder_config.json   # configuration sauvegardée (hôte/port TCP, snap, …)
```

---

## 7. Dépannage

```bash
# Le module est-il bien installé ?
ls -l /usr/lib/sdrpp/plugins/ | grep aprs_decoder

# Lancer SDR++ depuis un terminal pour voir les logs de chargement du module
sdrpp

# Réinitialiser la configuration du module si besoin
rm ~/.config/sdrpp/aprs_decoder_config.json
```

- **« given target ... already exists »** au `cmake` : vous avez un doublon dans
  le `CMakeLists.txt` racine (patch appliqué deux fois à la main). Vérifier :
  ```bash
  grep -c "OPT_BUILD_APRS_DECODER" ~/SDRPlusPlus/CMakeLists.txt        # attendu : 3
  grep -c "decoder_modules/aprs_decoder" ~/SDRPlusPlus/CMakeLists.txt  # attendu : 1
  ```
- **Le module n'apparaît pas dans le Module Manager** : vérifier que
  `aprs_decoder.so` est bien dans `/usr/lib/sdrpp/plugins/` et relancer
  `sudo ldconfig`.

---

73 de **F4JTV — ADRASEC 06**
