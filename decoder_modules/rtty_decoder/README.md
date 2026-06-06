# rtty_decoder — Module RTTY (Baudot/ITA2) pour SDR++

Module de décodage RTTY pour SDR++ (fork officiel d'Alexandre Rouma), compatible
avec les modes RTTY de FLDIGI. Décode le télétype Baudot/ITA2 (CCITT-2 / US-TTY)
avec démodulation FSK à deux tons (mark/space).

Conçu pour Ubuntu 24 et calqué **exactement** sur l'UI de tes modules
`psk_decoder` et `mfsk_decoder` : sélecteur de mode, sélecteur de bande latérale,
afficheur de bande interactif (retour visuel de l'AF + clic/glisser pour régler
l'AF), AFC, slider d'AF, et squelch.

## Caractéristiques

- **Démodulation FSK** par deux corrélateurs glissants (NCO mark/space, intégration
  boxcar sur un symbole), décision sur l'amplitude des deux tons.
- **Décodage Baudot/ITA2** complet : bascule LTRS/FIGS (0x1F / 0x1B), USOS
  (Unshift On Space) optionnel, gestion de l'espace en mode FIGS.
- **8 presets** FLDIGI courants : 45.45/170, 50/170, 75/170, 100/170, 45.45/85,
  75/850, 100/425, et **50/450 (DWD météo marine)** (baud/shift en Hz).
- **Bandes latérales** : USB (REF_LOWER), LSB (REF_UPPER, conjugaison du signal),
  NFM (REF_CENTER, discriminateur de phase).
- **Retour visuel de l'AF** : afficheur de spectre de bande (DFT 128 points sur
  0–3000 Hz) avec bande ombrée à la largeur du signal (shift + baud) et marqueur
  d'AF ; clic/glisser pour positionner l'AF, **identique** au widget PSK/MFSK.
- **AFC** : verrouillage automatique sur le point milieu de la paire de pics
  mark/space équilibrée (~shift d'écart). Lissage EMA. Affiche « (auto : X Hz) ».
- **Reverse** (inversion mark/space) et **USOS** réglables.
- **Squelch** de puissance (-100..0 dB).
- Sortie texte dans une fenêtre déroulante (autoscroll, plafond 20000 caractères,
  bouton Clear).

## Compilation

1. Copie le dossier `rtty_decoder/` dans `decoder_modules/` de l'arbre des sources
   SDR++ :

   ```
   SDRPlusPlus/decoder_modules/rtty_decoder/
   ```

2. Dans le `CMakeLists.txt` **racine** de SDR++, ajoute l'option et le
   sous-répertoire (à placer près des autres `decoder_modules`, après `radio`) :

   ```cmake
   option(OPT_BUILD_RTTY_DECODER "Build RTTY decoder" ON)
   # ...
   if (OPT_BUILD_RTTY_DECODER)
       add_subdirectory("decoder_modules/rtty_decoder")
   endif ()
   ```

3. Build (depuis un dossier `build/` à la racine) :

   ```bash
   cd build
   cmake .. -DOPT_BUILD_RTTY_DECODER=ON
   make -j$(nproc) rtty_decoder
   ```

4. Installe le module compilé. Selon ton installation :

   ```bash
   # installation système classique
   sudo cp decoder_modules/rtty_decoder/rtty_decoder.so /usr/lib/sdrpp/plugins/

   # ou exécution depuis l'arbre de build : le .so se trouve dans
   # build/decoder_modules/rtty_decoder/rtty_decoder.so
   ```

5. Dans SDR++ : **Module Manager** → ajoute une instance de type `rtty_decoder`
   (nom au choix, ex. `RTTY`). Le module apparaît dans le menu de gauche.

## Utilisation

1. Choisis **USB** comme bande latérale (cas habituel en RTTY décalé), puis
   positionne le VFO sur le signal RTTY dans le waterfall.
2. Place l'**AF** sur les deux tons RTTY : clique/glisse directement dans
   l'afficheur de bande, ou utilise le slider « AF freq » (700–2500 Hz). La bande
   ombrée doit recouvrir les deux raies mark/space.
3. Active l'**AFC** pour le verrouillage automatique : le module suit la paire de
   pics et affiche la fréquence centrale détectée. (Les premiers caractères
   peuvent être brouillés pendant l'acquisition ~1–2 s, c'est normal.)
4. Sélectionne le **preset** correspondant (45.45/170 pour le RTTY amateur
   standard).
5. Si le texte est incohérent (lettres/chiffres inversés ou flux inversé), active
   **Reverse**. Active/désactive **USOS** selon l'émetteur.
6. Le texte décodé s'affiche dans la fenêtre en bas ; **Clear** vide le tampon.

### Météo marine (DWD Pinneberg)

Pour recevoir la météo marine du Deutscher Wetterdienst (émetteur de Pinneberg,
fréquences 11039 / 14467,3 / 4583 / 7646 / 10100,8 kHz) :

- Bande latérale : **USB**, VFO calé sur le signal (ex. ~7645,4 kHz pour 7646 kHz).
- Preset : **50 baud / 450 Hz (DWD meteo)**.
- Active l'**AFC** (la paire de tons à 450 Hz d'écart est verrouillée
  automatiquement).
- USOS conseillé activé.

## Validation

Le cœur DSP a été validé sur signaux RTTY synthétiques :

| Test                         | Résultat |
|------------------------------|----------|
| USB 45.45/170 « RYRY DE F4JTV K » | PASS |
| LSB                          | PASS |
| Reverse                      | PASS |
| 75 baud                      | PASS |
| AFC (verrouillage auto)      | PASS (premiers caractères brouillés pendant l'acquisition) |

## Structure

```
rtty_decoder/
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp            # Module : VFO, UI (identique PSK/MFSK), cycle de vie
    ├── decoder.h           # Interface abstraite Decoder
    └── rtty/
        ├── decoder.h       # RTTYDecoder : squelch → sink → modem, UI mode
        ├── modem.h         # Modem FSK + trame UART + AFC + spectre de bande
        └── baudot.h        # Tables ITA2/CCITT-2 + décodage Baudot
```
