# parse_photon_jsonl_dump

Liest einen komprimierten [Photon](https://github.com/komoot/photon)-Dump, baut daraus einen
Suchindex für Postadressen und legt ihn als Binärdatei ab, die beim Start nur noch
gemappt wird.

```
photon_dump.jsonl.zst  ──►  parse_photon_jsonl_dump  ──►  index.gdx  ──►  mmap
      24 GB                     zwei Durchläufe                            < 1 ms
```

## Was es tut

1. **Entpackt und liest** den zstd-komprimierten Dump im Strom.
2. **Parst** jede JSON-Zeile mit [yyjson](https://github.com/ibireme/yyjson) und trennt pro
   Eintrag zwei Dinge: die Felder, die in einer Antwort stehen (Straße, Hausnummer, PLZ,
   Ort, Koordinate, `importance`), und den rollenfreien Suchtext.
3. **Faltet und zerlegt** den Suchtext: Kleinschreibung, Diakritika, `ß → ss`, Umlaute in
   beiden Schreibweisen (`ü → ue` und `ü → u`), Abkürzungen (`str. → strasse`,
   `St. → Sankt`) und Komposita (`superstrasse → super + strasse`).
4. **Sammelt, sortiert und dedupliziert** die Wörter — pro Thread lock-frei, nach Prefix
   gruppiert, am Ende in einem k-Way-Merge zusammengeführt. Daneben entsteht ein zweites
   Wörterbuch mit den Originalschreibweisen für die Ausgabe.
5. **Läuft ein zweites Mal** über den Dump und schreibt Dokumente (ein Ort, eine
   Koordinate, ein Gewicht) und Posting-Listen (Wort → Dokumente). Ein Posting braucht
   den Rang seines Wortes, und den gibt es erst, wenn alle Wörter sortiert sind.
6. **Läuft ein drittes Mal** und hängt die Hausnummern an ihre Straßen. Auch das geht
   erst, wenn die Straßen Dokumente mit Nummern sind — und ein Durchlauf kostet Minuten,
   während das Zwischenspeichern von 292 Millionen Häusern Gigabyte kostet.
7. **Schreibt** das Ergebnis als `.gdx`-Datei in genau der Form, in der es gelesen wird.

Was eine Hausnummer trägt, ist eine Adresse — unabhängig davon, was der `address_type`
sagt. Der Dump führt ein Ferienlager mit Nummer unter `other` und ein Schiffswrack ohne
Nummer unter `house`. Was keine Nummer hat und zu keiner Ebene der Adresshierarchie
gehört, ist ein Teich oder ein Radweg und bleibt draußen.

## Aufruf

```sh
parse_photon_jsonl_dump <photon_dump.jsonl.zst> [index.gdx] [parser_threads]
parse_photon_jsonl_dump <index.gdx> ["Suchanfrage"] [max_treffer]
```

Die Dateiendung entscheidet über den Weg: endet der erste Parameter auf `.gdx`, wird ein
fertiger Index geladen, sonst wird aus dem Dump gebaut.

| Argument | Beschreibung | Vorgabe |
| --- | --- | --- |
| `photon_dump.jsonl.zst` | Quelldump | erforderlich |
| `index.gdx` | Zieldatei beim Bauen | aus dem Dumpnamen abgeleitet |
| `parser_threads` | Parser-Threads (1–10) | 4 |
| `"Suchanfrage"` | Wörter in beliebiger Reihenfolge | ohne: nur Kennzahlen |
| `max_treffer` | Zahl der gezeigten Treffer | 10 |

```sh
# bauen: planet.jsonl.zst -> planet.gdx
parse_photon_jsonl_dump planet.jsonl.zst 8

# laden und Kennzahlen zeigen
parse_photon_jsonl_dump planet.gdx

# suchen — Reihenfolge egal, Abkürzungen und Schreibweisen werden gefaltet
parse_photon_jsonl_dump planet.gdx "Berlin, Superstr. 8"
parse_photon_jsonl_dump planet.gdx "15328 Bleyen" 5
```

Die Anfrage geht durch dieselbe Faltung wie der Index: `Superstr.`, `superstrasse` und
`SUPERSTRASSE` treffen dasselbe Wort, `München` findet sich auch als `Muenchen` oder
`Munchen`. Gefunden wird, wo alle Wörter der Anfrage zusammentreffen; Wörter, die der
Index nicht kennt, werden übergangen statt die ganze Anfrage scheitern zu lassen.
Sortiert wird nach Photons eigenem `importance`-Gewicht; wer nach einer Hausnummer
fragt, bekommt die Straße, die sie trägt, zuerst.

Eine Zahl in der Anfrage ist zuerst eine Hausnummer und erst danach ein Wort: `Superstraße 8`
sucht die Straße ohne die 8 und löst die Nummer dort auf. Nur wenn das nichts findet,
darf die Zahl als Wort auftreten — sonst würde `Straße des 17. Juni` scheitern.

## Kennzahlen

Gemessen auf dem Planeten-Dump (24,21 GB) und dem Deutschland-Auszug (2,10 GB):

| | Deutschland | Welt |
| --- | --- | --- |
| Einträge | 26,7 M | 356,3 M |
| Orte (Dokumente) | 1,6 M | 34,7 M |
| Hausnummern | 20,6 M | 248,6 M |
| eindeutige Wörter | 594 k | 7,83 M |
| Indexdatei | 315 MB | 4,8 GB |
| Bauzeit | 1,1 min | 15,4 min |
| Öffnen | 0,25 ms | 0,26 ms |

Die Bauzeit hängt am Dekomprimieren — dreimal derselbe Dump. Alles andere zusammen
kostet auf dem Planeten gut eine Minute.

## Bauen

```sh
zig build -Dtarget=x86_64-linux-gnu
```

Das Binary landet unter `./zig-out/bin/parse_photon_jsonl_dump`.

Abhängigkeiten holt das Zig-Buildsystem selbst:
[zstd](https://github.com/facebook/zstd),
[gradido-blockchain-core](https://github.com/gradido/gradido-blockchain-core) (Bucket-Vektor,
Arena-Allokator, Timer), [yyjson](https://github.com/ibireme/yyjson) und
[CRoaring](https://github.com/RoaringBitmap/CRoaring) (beide mitgeliefert).

Voraussetzungen: Zig ≥ 0.15.1, pthreads, Linux.

## Architektur

```
┌──────────────┐     ┌──────────────┐     ┌────────────────┐
│  Hauptthread │────►│  ParseQueue  │────►│ Parser-Threads │
│  zstd-Strom  │     │  (begrenzt)  │     │ yyjson + Falten│
└──────────────┘     └──────────────┘     └────────┬───────┘
                                                   │ je Thread: sortieren
                                          ┌────────▼────────┐
                                          │  k-Way-Merge    │
                                          │  über Prefixe   │
                                          └────────┬────────┘
                                          ┌────────▼────────┐
                                          │  geo_index_write│
                                          └─────────────────┘
```

| Modul | Aufgabe |
| --- | --- |
| `main` | Kommandozeile, zstd-Strom, Threads, drei Durchläufe |
| `json_parse` | Photon-Zeilen → `PhotonPlace` (Anzeigefelder + Suchtexte) |
| `text_tokenize` | Falten, Abkürzungen, Komposita — Index und Anfrage gehen denselben Weg |
| `name_collector` | Sammeln, sortieren, deduplizieren; k-Way-Merge über die Threads |
| `prefix_tree` | Byte-weiser Indexbaum: ein Zeichen je Ebene, am Ende ein Index |
| `doc_collector` | Dokumente und Postings je Thread, Zusammenführung per Zählsortierung |
| `house_collector` | Hausnummern an ihren Straßen, geordnet je Straße |
| `geo_index` | Dateiformat, Writer, `mmap`-Loader, Wortsuche, Anfragen über Bitmaps |
| `meta_area_allocator` | Arena-Ketten für die Textbytes, eine je Thread |
| `parse_queue`, `line_buffer`, `progress`, `format`, `error` | Infrastruktur |

## Dateiformat

```
[ header          ] Magic, Version, Layout-Hash, Byte-Order, Zählwerte
[ sections        ] je Sektion: Art, Offset, Länge
[ Wörterbuch      ] groups, offsets, text — gefaltete Wörter, was eine Anfrage trifft
[ Schreibweisen   ] groups, offsets, text — Original, was eine Antwort zeigt
[ documents       ] je Ort ein fester Satz: Koordinate, Gewicht, Typ, Anzeige-Ränge
[ posting offsets ] word_count + 1 Byte-Offsets in die Postings
[ postings        ] je Wort eine eingefrorene Roaring-Bitmap, 32-Byte-ausgerichtet
```

Zwei Wörterbücher, weil die beiden Aufgaben sich widersprechen: eine Anfrage tippt
*muenchen*, eine Antwort muss *München* lesen. Gefaltete Wörter teilen sich weit
häufiger als geschriebene, deshalb bleibt die Suchseite klein.

Die Postings sind Roaring-Bitmaps ([CRoaring](https://github.com/RoaringBitmap/CRoaring),
mitgeliefert): ein Wort auf Millionen Orten kostet damit ein Bit je Ort statt vier Byte,
und die Frage „steht dieser Ort darauf" ist ein Bit-Test statt einer Suche durch Gigabyte.
Das *frozen*-Format ist das Speicherabbild selbst — beim Öffnen wird nichts dekodiert,
eine Bitmap wird dort betrachtet, wo sie liegt.

Keine Zeiger, nur `uint32`-Indizes und Offsets; feste Feldbreiten mit `static_assert`;
Header prüft Magic, Version, Byte-Reihenfolge, Layout-Hash und Dateigröße, bevor ein Byte
gelesen wird. Deshalb ist Laden ein `mmap` und kostet unabhängig von der Dateigröße nichts.

## Entwicklung

```sh
./lint.sh     # clang-format über src/
doxygen       # API-Dokumentation
```

Kommentare folgen dem Zwei-Schichten-Standard aus [AGENTS.md](AGENTS.md): präzise
technische Spezifikation, dazu wo passend eine `@whisper`-Zeile.

## Lizenz

Siehe die Lizenzdateien in `third_party/` für die mitgelieferten Abhängigkeiten.
