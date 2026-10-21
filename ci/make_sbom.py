#!/usr/bin/env python3
"""Produce UNA SBOM SPDX 2.2 di tipo dichiarato, dall'output multi-documento di Yocto.

Si chiamava make_sbom.py. Il nome e' cambiato perche' descriveva la meccanica invece
del risultato: la fusione e' il mezzo, non lo scopo. VulnScout l'archivio di Yocto se lo
scompatta da solo, quindi fondere non serve a lui; serve ad avere un file unico, con un
TIPO dichiarato e il filtro giusto applicato, da dare a un cliente o a un altro strumento.
Chi legge il vecchio nome conclude "a noi non serve" e butta via anche il filtro e
l'etichettatura, che sono la parte che conta.

VERSIONE PRELIMINARE, NON ANCORA CONTROLLATA.
=============================================
Scritta il 2026-10-20 per capire se la fusione fosse praticabile, non per
produrre un deliverable. Gira e produce un file, ma NESSUNO ha ancora
verificato che il risultato sia uno SPDX valido ne' che sia completo:

  - non e' stato passato a un validatore SPDX;
  - non e' stato aperto da nessuno strumento di composition analysis;
  - la scelta di quali relazioni tenere (--lean) e' una scelta nostra, non
    una regola dello standard;
  - i documenti "runtime-*" vengono assorbiti insieme agli altri senza un
    ragionamento su cosa significhino nel documento finale.

Prima di usarlo per qualcosa che esce dall'azienda, va validato.

Perche' esiste
--------------
Yocto kirkstone (poky 4.0.35) non produce una SBOM in un file solo. Produce
un documento per ricetta, uno per pacchetto e uno per runtime, piu' un
index.json, tutti dentro <immagine>.spdx.tar.zst. Sulla nostra immagine sono
3564 documenti. Il file <immagine>.spdx.json che sta accanto all'immagine non
e' autosufficiente: contiene un pacchetto e rimanda agli altri documenti con
externalDocumentRefs.

Su Yocto scarthgap esiste create-spdx-3.0, disattivato per difetto, che
produce gia' un documento consolidato. Se passiamo a scarthgap questo script
probabilmente non serve piu'.

Misure sulla nostra immagine, 2026-10-20
----------------------------------------
  documenti letti       3564
  pacchetti             2053
  relazioni           273448
  licenze estratte        79
  SPDXID in conflitto      0     <- nessuna collisione, non serve rinominare
  dimensione intera     62,9 MB
  dimensione --lean      7,3 MB

Gli 80% delle relazioni sono GENERATED_FROM, cioe' il legame fra ogni binario
e i sorgenti da cui nasce: informazione vera ma di archeologia della build.
--lean tiene solo CONTAINS, DESCRIBES e i loro inversi.

Uso
---
  tar --zstd -xf core-image-...spdx.tar.zst -C /tmp/spdx
  ./make_sbom.py /tmp/spdx -o merged.spdx.json
  ./make_sbom.py /tmp/spdx -o merged.spdx.json --lean
"""
import argparse, json, pathlib, sys

CONTENT_RELS = {"CONTAINS", "CONTAINED_BY", "DESCRIBES", "DESCRIBED_BY"}

# Pacchetti che vivono solo sulla macchina che compila e non finiscono sulla scheda:
# -native sono gli strumenti che Yocto si costruisce per se' (il suo openssl per firmare,
# il suo python per gli script, qemu per emulare), -cross e -crosssdk sono i compilatori
# incrociati. Sulla nostra immagine valgono oltre 1250 CVE di roba che sul prodotto non c'e'.
HOST_SUFFIXES = ("-native", "-cross", "-crosssdk", "-cross-canadian")
HOST_INFIXES = ("-cross-", "-crosssdk-", "-nativesdk-")


def is_host_only(name: str) -> bool:
    """Vero se il pacchetto appartiene all'ambiente di build, non all'immagine."""
    if not name:
        return False
    return name.endswith(HOST_SUFFIXES) or any(i in name for i in HOST_INFIXES) \
        or name.startswith("nativesdk-")


# I tre tipi della tassonomia CISA che ci riguardano. SPDX 2.2 non ha un campo per
# dichiarare di che tipo e' una SBOM, quindi lo diciamo nel nome del documento e nel
# commento di creationInfo: tre file senza tipo dichiarato sono peggio di un file solo,
# perche' chi li riceve non sa quale guardare e finisce per confrontarli fra loro.
SBOM_TYPES = {
    "build": ("Build SBOM",
              "Generated during the build. Includes the build-time dependencies, so it lists "
              "host tools (-native, -cross) that are not present on the device."),
    "deployed": ("Deployed SBOM",
                 "Inventory of what is installed on the device. Host build tools (-native, "
                 "-cross) are excluded: they run on the build machine and are not shipped."),
}


def load_installed(path):
    """L'elenco esatto dei pacchetti installati nell'immagine, dal buildhistory.

    E' verita' di fatto, non un'euristica sul nome: Yocto lo scrive sapendo cosa ha
    messo davvero nel rootfs. Sta in
      build/buildhistory/images/<machine>/<libc>/<immagine>/installed-package-names.txt
    e finisce nell'artefatto dei log della CI.
    """
    return {l.strip() for l in open(path) if l.strip()}


def merge(src: pathlib.Path, name: str, lean: bool, sbom_type: str, installed=None):
    index = json.load(open(src / "index.json"))["documents"]
    packages, rels, lic, dropped = [], [], {}, []
    seen, collisions, read = set(), 0, 0

    for entry in index:
        f = src / entry["filename"]
        if not f.exists():
            continue
        doc = json.load(open(f))
        read += 1

        for p in doc.get("packages", []):
            pid = p.get("SPDXID")
            if pid in seen:
                collisions += 1
                continue
            if sbom_type == "deployed":
                pname = p.get("name") or ""
                # Con l'elenco del buildhistory si tiene cio' che e' davvero installato.
                # Senza, si ripiega sui nomi, che e' un'approssimazione: prende i -native
                # e i -cross ma lascia dentro i -dev e i -dbg, che sono costruiti e non
                # installati, e scarta per sbaglio ricette che si chiamano -cross senza
                # esserlo (depmodwrapper-cross, qemuwrapper-cross, libtool-cross).
                drop = pname not in installed if installed is not None else is_host_only(pname)
                if drop:
                    dropped.append(pname)
                    continue
            seen.add(pid)
            packages.append(p)

        for r in doc.get("relationships", []):
            if lean and r.get("relationshipType") not in CONTENT_RELS:
                continue
            # "DocumentRef-xxx:SPDXRef-yyy" -> "SPDXRef-yyy": gli SPDXID di Yocto
            # sono gia' univoci fra documenti, quindi basta togliere il prefisso.
            for k in ("spdxElementId", "relatedSpdxElement"):
                v = r.get(k, "")
                if isinstance(v, str) and v.startswith("DocumentRef-") and ":" in v:
                    r[k] = v.split(":", 1)[1]
            rels.append(r)

        for e in doc.get("hasExtractedLicensingInfos", []):
            prev = lic.get(e["licenseId"])
            # tieni la voce che ha davvero il testo: un documento puo' dichiarare
            # la licenza con extractedText nullo e un altro con il testo vero.
            if prev is None or (not prev.get("extractedText") and e.get("extractedText")):
                lic[e["licenseId"]] = e

    empty = [k for k, v in lic.items() if not v.get("extractedText")]
    label, meaning = SBOM_TYPES[sbom_type]
    comment = (
        f"SBOM type: {label} (CISA taxonomy). {meaning} "
        f"Merged from {read} SPDX documents produced by Yocto create-spdx. "
        "PRELIMINARY, NOT VALIDATED: see tools/make_sbom.py."
    )
    if sbom_type == "deployed":
        comment += f" {len(dropped)} host-only packages were excluded."
    if lean:
        comment += (" Only content relationships are kept; the build-provenance "
                    "relationships (GENERATED_FROM, BUILD_DEPENDENCY_OF) are dropped.")
    return {
        "spdxVersion": "SPDX-2.2",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{name} ({sbom_type})",
        "documentNamespace": f"http://spdx.org/spdxdoc/{name}-{sbom_type}",
        "creationInfo": {
            "created": "1970-01-01T00:00:00Z",
            "creators": ["Tool: sysele-merge-spdx-preliminary",
                         "Organization: System Electronics"],
            "comment": comment,
        },
        "packages": packages,
        "relationships": rels,
        "hasExtractedLicensingInfos": list(lic.values()),
    }, dict(read=read, packages=len(packages), rels=len(rels),
            lic=len(lic), collisions=collisions, empty=empty, dropped=dropped)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", type=pathlib.Path, help="cartella con i documenti estratti")
    ap.add_argument("-o", "--out", type=pathlib.Path, default=pathlib.Path("merged.spdx.json"))
    ap.add_argument("-n", "--name", default="merged")
    ap.add_argument("-t", "--type", choices=sorted(SBOM_TYPES), default="build",
                    help="build = tutto, come lo genera Yocto; deployed = solo cio' che "
                         "sta sulla scheda, senza gli strumenti dell'host")
    ap.add_argument("--installed", type=pathlib.Path,
                    help="installed-package-names.txt del buildhistory: con questo il tipo "
                         "deployed tiene esattamente cio' che e' nell'immagine")
    ap.add_argument("--lean", action="store_true",
                    help="tieni solo le relazioni di contenuto, non la storia della build")
    a = ap.parse_args()

    if not (a.src / "index.json").exists():
        sys.exit(f"{a.src}/index.json non c'e': estrai prima il .spdx.tar.zst")

    installed = load_installed(a.installed) if a.installed else None
    if a.type == "deployed" and installed is None:
        print("  nota: senza --installed il filtro va per nome, che e' approssimato")
    doc, st = merge(a.src, a.name, a.lean, a.type, installed)
    a.out.write_text(json.dumps(doc, indent=2))

    print(f"  tipo                 {SBOM_TYPES[a.type][0]}")
    print(f"  documenti letti      {st['read']}")
    print(f"  pacchetti            {st['packages']}")
    print(f"  relazioni            {st['rels']}")
    print(f"  licenze estratte     {st['lic']}")
    print(f"  SPDXID in conflitto  {st['collisions']}")
    print(f"  dimensione           {a.out.stat().st_size/1048576:.1f} MB")
    if st["dropped"]:
        print(f"  esclusi (solo host)  {len(st['dropped'])}")
    if st["empty"]:
        print(f"  ATTENZIONE: {len(st['empty'])} licenze senza testo: {', '.join(st['empty'][:6])}")
        print("     una licenza senza testo rende la SBOM incompleta proprio sui")
        print("     componenti proprietari. Vedi knowledge/sbom.md.")


if __name__ == "__main__":
    main()
