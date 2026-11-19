# Fremder Code und fremde Daten / Third-party code and data

Diese Teile stammen nicht aus Simple Open Camper und stehen unter ihren eigenen Lizenzen – nicht unter der
Projektlizenz (PolyForm Noncommercial). / These parts are not from Simple Open Camper and keep their own licences –
not the project licence (PolyForm Noncommercial).

| Pfad / Path | Herkunft / Origin | Lizenz / Licence |
|---|---|---|
| `micro-ecc/` (eingebunden über `uecc_ef.c`, `uecc_p256.c`) | [kmackay/micro-ecc](https://github.com/kmackay/micro-ecc) | BSD-2-Clause, siehe `micro-ecc/LICENSE.txt` |
| `ecoflow/ef_keydata.h` | Schlüsseltabelle aus [rabits/ha-ef-ble](https://github.com/rabits/ha-ef-ble) (`eflib/keydata.py`) | Apache-2.0, siehe `ecoflow/LICENSE-Apache-2.0.txt` |

`uecc_ef.c/.h` und `uecc_p256.c/.h` sind kleine eigene Hüllen (Namenspräfix, Einstellungen) um micro-ecc.
`uecc_ef.c/.h` and `uecc_p256.c/.h` are small wrappers of our own (name prefix, settings) around micro-ecc.
