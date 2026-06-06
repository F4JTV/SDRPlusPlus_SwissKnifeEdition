# psk_decoder

Module SDR++ pour décoder en direct les modes PSK numériques de type
FLDIGI / MultiPsk : familles BPSK, QPSK et 8PSK.

Maintenu par **F4JTV** (ADRASEC 06).

## Modes supportés

### Famille BPSK

| Mode     | Baud    | Largeur ~ | FEC               |
|----------|---------|-----------|-------------------|
| BPSK31   | 31.25   | 62 Hz     | non               |
| BPSK63   | 62.5    | 160 Hz    | non               |
| BPSK63F  | 62.5    | 140 Hz    | Viterbi K=7 R=1/2 |
| BPSK125  | 125     | 250 Hz    | non               |
| BPSK250  | 250     | 500 Hz    | non               |
| BPSK500  | 500     | 1000 Hz   | non               |
| BPSK1000 | 1000    | 2000 Hz   | non               |

### Famille QPSK

| Mode    | Baud    | Largeur ~ | FEC               |
|---------|---------|-----------|-------------------|
| QPSK31  | 31.25   | 62 Hz     | Viterbi K=5 R=1/2 |
| QPSK63  | 62.5    | 160 Hz    | Viterbi K=5 R=1/2 |
| QPSK125 | 125     | 250 Hz    | Viterbi K=5 R=1/2 |
| QPSK250 | 250     | 500 Hz    | Viterbi K=5 R=1/2 |
| QPSK500 | 500     | 1000 Hz   | Viterbi K=5 R=1/2 |

### Famille 8PSK

| Mode     | Baud    | Largeur ~ | FEC |
|----------|---------|-----------|-----|
| 8PSK125  | 125     | 250 Hz    | non |
| 8PSK250  | 250     | 500 Hz    | non |
| 8PSK500  | 500     | 1000 Hz   | non |
| 8PSK1000 | 1000    | 2000 Hz   | non |

Les variantes 8PSK*F (avec FEC convolutionnel + interleaver) et 8PSK*FL
(avec pilot tone) ne sont pas implémentées dans cette version.

### Détails FEC

- **BPSK63F** : code convolutionnel K=7 R=1/2, polynômes NASA G1=0x79, G2=0x5B,
  encodés interleavés (G1, G2, G1, G2, ...). Décodage Viterbi hard-decision
  avec traceback 64.
- **QPSK** : code convolutionnel K=5 R=1/2, polynômes G3PLX/PSK31 G1=0x17,
  G2=0x19. Décodage Viterbi hard-decision avec traceback 32. Les 2 bits par
  symbole QPSK sont la sortie directe de l'encodeur convolutionnel.
- **8PSK basique** : pas de FEC. Les 3 bits par symbole sont mappés en Gray
  code et envoyés directement au décodeur Varicode.

## VFO

La géométrie de la VFO s'adapte automatiquement au mode VFO sélectionné :

| Mode VFO | Référence  | Bande passante | Position du passband     |
|----------|------------|----------------|--------------------------|
| USB      | REF_LOWER  | 2800 Hz        | À DROITE du curseur dial |
| LSB      | REF_UPPER  | 2800 Hz        | À GAUCHE du curseur dial |
| NFM      | REF_CENTER | 12500 Hz       | Centrée sur le curseur   |

Le sample rate audio interne est fixé à 24 kHz pour permettre les 12.5 kHz
de canal NFM (Nyquist). Les modes SSB ne consomment que ~3 kHz de cette bande.

## Squelch

Un *Power Squelch* (identique à celui du module Radio de SDR++) gate la
chaîne IQ avant démodulation. Quand la puissance moyenne dans la VFO descend
sous le seuil, le décodeur ne reçoit que des zéros et n'avance pas.

## Auto-tune AF

Le bouton **Auto AF** lance un scan de 2.5 secondes sur la bande passante
audio (500–2700 Hz) pour détecter automatiquement la fréquence centrale du
signal PSK. La technique est inspirée de l'auto-acquisition du module MFSK :

1. Banc Goertzel sur une grille de 221 candidats (pas 10 Hz)
2. Accumulation sur 25 segments de 0.1 s (méthode de Welch) — la longueur de
   segment est calibrée pour que la largeur de bin Goertzel = pas de la grille
3. Localisation du pic par centroide pondéré sur la région > 1/4 max
   (gère correctement les signatures BPSK idle à 2 sidebands)

Tolérance au bruit validée jusqu'à -10 dB SNR avec précision sub-Hz.
Pendant le scan, une barre de progression remplace le bouton ; le décodage
continue normalement (le bloc auto-tune est un pass-through transparent).

## Band view (réglage visuel AF)

Au-dessus du slider AF freq, un mini-spectre en temps réel affiche la bande
audio (500–2700 Hz) avec :

- Les **barres bleues** : magnitude du signal audio par bin (10 Hz)
- La **bande bleue translucide** : largeur attendue du signal autour de l'AF
  courant (≈ baud rate pour BPSK/QPSK, 1.5 × baud pour 8PSK)
- Le **trait jaune vertical** : position actuelle du marqueur AF (équivalent
  curseur rouge FLDIGI)

**Cliquer/glisser** dans le widget règle l'AF directement sur la position
souris. Le spectre est rafraîchi 10 fois par seconde avec lissage, donc on
voit clairement les sidebands d'un BPSK idle, la "bosse" d'un signal QPSK
ou 8PSK actif, et tout signal parasite. Inspiré directement du widget
équivalent dans le module MFSK.

## Installation

Activer l'option CMake `OPT_BUILD_PSK_DECODER` (activée par défaut) puis :

```bash
cd build
cmake ..
make psk_decoder -j$(nproc)
```

Le fichier `decoder_modules/psk_decoder/psk_decoder.so` est ensuite à copier
dans le dossier des modules SDR++ (typiquement `/usr/lib/sdrpp/plugins/`) et à
ajouter via *Module Manager > Add* (nom `psk_decoder`).

**Migration depuis l'ancien `fldigi_decoder`** : retirer l'ancienne instance
du Module Manager, supprimer l'ancien `fldigi_decoder.so` et l'ancien fichier
`fldigi_decoder_config.json` (optionnel — il sera juste orphelin).
La nouvelle instance sauvegarde dans `psk_decoder_config.json`.

## Utilisation

1. Choisir le mode VFO : **USB** (HF par défaut), **LSB**, ou **NFM** (VHF/UHF).
2. Faire glisser la VFO sur la waterfall pour la mettre sur le signal.
3. Régler le **Snap** si nécessaire.
4. Sélectionner le mode souhaité (BPSK / QPSK / 8PSK + vitesse).
5. Cliquer **Auto AF** pour détecter automatiquement la fréquence audio du
   signal, ou ajuster manuellement avec le slider **AF freq** (700–2500 Hz).
6. Le scope affiche le verrouillage de la démodulation. Le texte décodé
   apparaît quand le démodulateur a verrouillé.
7. Ajuster le **Squelch** pour ne pas décoder le bruit.

## Caveats

- **BPSK63F**, **QPSK*** et **8PSK*** : la compatibilité bit-à-bit avec un
  émetteur FLDIGI réel n'a pas été validée sur signal RF pour les modes
  avec FEC ou avec constellation > 2. Les implémentations suivent les
  spécifications publiques (polynômes G3PLX/NASA, mapping Gray standard).
  Si le décodage échoue alors que la constellation est bien verrouillée,
  c'est possiblement une convention de bit ordering / Gray mapping inversée.
- **8PSK** demande un SNR élevé et une bonne stabilité de phase. Sans FEC,
  les modes basiques ne tolèrent pas le bruit. Pour de l'opération HF réelle,
  les variantes 8PSK*F (avec FEC) sont conseillées — pas encore implémentées.
- Les modes ≥ baud 500 demandent un signal très propre. Les gains des
  boucles Costas/M&M sont ouverts pour ces vitesses.

## Architecture interne

```
VFO → PowerSquelch → SSB ou NFM demod → AutoTuner (pass-through)
  → ToneMixer (NCO @ -AF freq) → RationalResampler → PSK<N> demod
  → handler → différentiel → bits → [Viterbi] → Varicode → texte
```

| N  | Decoder       | FEC                       |
|----|---------------|---------------------------|
| 2  | BPSKDecoder   | Viterbi K=7 (BPSK63F seul)|
| 4  | QPSKDecoder   | Viterbi K=5 (toutes)      |
| 8  | PSK8Decoder   | aucun                     |

Fichiers :
- `src/psk/decoder.h` — BPSKDecoder + profils BPSK
- `src/psk/qpsk_decoder.h` — QPSKDecoder + profils QPSK
- `src/psk/psk8_decoder.h` — PSK8Decoder + profils 8PSK
- `src/common/viterbi_k7.h` — Viterbi K=7 R=1/2 (BPSK63F)
- `src/common/viterbi_k5.h` — Viterbi K=5 R=1/2 (QPSK)
- `src/common/varicode.h` — décodeur Varicode PSK31
