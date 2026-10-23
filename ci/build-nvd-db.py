#!/usr/bin/env python3
"""Costruisce il database CVE di Yocto dai feed della community invece che dalle API NIST.

Le API del NIST servono il database a pagine da 4 MB, oltre duecento, e interrompono le
connessioni sulle risposte lunghe: un download completo non arriva in fondo entro il
tempo di un runner. fkie-cad/nvd-json-data-feeds pubblica gli stessi dati come file
annuali compressi, 222 MB in tutto, sulla CDN di GitHub.

Non riscrive la logica di poky: ESTRAE initialize_db, parse_node_and_insert e update_db
dalla ricetta cve-update-nvd2-native.bb e le esegue. Lo schema e il contenuto sono quindi
identici per costruzione, e vanno dietro a poky senza modifiche. La ricetta va passata con
--recipe, e deve essere quella della stessa poky che poi leggera' il database.

Il mirror e' un progetto della community, senza licenza dichiarata. Ragionevole per
costruire il database; da rivalutare quando si potra' tornare alla fonte ufficiale.

DI QUESTO FILE ESISTONO DUE COPIE: questa, che esegue la CI (cve-db.yml), e una di comodo
in WSL sotto workspace/cyber_security/. Se ne modifichi una, porta la modifica anche
nell'altra.

  ./build-nvd-db.py -r <ricetta.bb> -o nvdcve_2-2.db [--cache <cartella>]
"""
import argparse, io, json, lzma, pathlib, re, sqlite3, sys, urllib.request, datetime

RELEASE = "https://api.github.com/repos/fkie-cad/nvd-json-data-feeds/releases/latest"


def poky_functions(recipe):
    """Estrae dalla ricetta le funzioni pure che costruiscono il database.

    Sono Python normale che lavora su una connessione sqlite3: non toccano il datastore
    di bitbake, quindi girano anche fuori da una build.
    """
    lines = recipe.read_text().split("\n")
    wanted = ("initialize_db", "parse_node_and_insert", "update_db")
    out = []
    for name in wanted:
        try:
            i = next(n for n, l in enumerate(lines) if l.startswith(f"def {name}("))
        except StopIteration:
            sys.exit(f"funzione {name} non trovata in {recipe.name}: la ricetta e' cambiata")
        # il corpo e' tutto cio' che segue finche' resta indentato o vuoto. Taglia qui
        # invece che su un'espressione regolare, perche' dopo le funzioni la ricetta ha
        # costrutti bitbake di ogni forma (do_fetch[nostamp], EXCLUDE_FROM_WORLD, ...).
        j = i + 1
        while j < len(lines) and (lines[j].startswith((" ", "\t")) or not lines[j].strip()):
            j += 1
        out.append("\n".join(lines[i:j]))
    # update_db usa bb.note per i CVE senza configurazioni. Fuori da bitbake non
    # esiste, quindi gli diamo un surrogato silenzioso: sono migliaia di righe che
    # nel nostro contesto non dicono nulla.
    class _bb:
        @staticmethod
        def note(*a, **k): pass
        warn = debug = plain = note
        @staticmethod
        def error(*a, **k): print("  bb.error:", *a)

    ns = {"sqlite3": sqlite3, "json": json, "datetime": datetime, "bb": _bb}
    exec("\n".join(out), ns)
    return ns


def assets():
    with urllib.request.urlopen(RELEASE, timeout=60) as r:
        rel = json.load(r)
    # Solo i file annuali. Il mirror pubblica anche CVE-all (tutto), CVE-modified e
    # CVE-recent (finestre recenti), che si sovrappongono agli annuali: processarli
    # insieme duplica le righe in PRODUCTS, che non ha vincoli di unicita', e
    # cve-check finirebbe per segnalare lo stesso CVE due volte per lo stesso pacchetto.
    per_year = re.compile(r"^CVE-\d{4}\.json\.xz$")
    names = [(a["name"], a["browser_download_url"]) for a in rel["assets"]
             if per_year.match(a["name"])]
    return rel["tag_name"], sorted(names)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", type=pathlib.Path, required=True)
    ap.add_argument("-r", "--recipe", type=pathlib.Path, required=True,
                    help="cve-update-nvd2-native.bb della stessa poky che poi leggera' "
                         "il database: le funzioni di import vengono estratte da li'")
    ap.add_argument("--cache", type=pathlib.Path, default=pathlib.Path("nvd-feeds"))
    a = ap.parse_args()

    fns = poky_functions(a.recipe)
    tag, files = assets()
    print(f"  release del mirror: {tag}, {len(files)} file annuali")
    a.cache.mkdir(parents=True, exist_ok=True)
    a.out.parent.mkdir(parents=True, exist_ok=True)

    tmp = a.out.with_suffix(".db.building")
    tmp.unlink(missing_ok=True)
    conn = sqlite3.connect(tmp)
    fns["initialize_db"](conn)

    total = 0
    for name, url in files:
        local = a.cache / name
        if not local.exists():
            # timeout esplicito: senza, una connessione che si impianta blocca per sempre.
            with urllib.request.urlopen(url, timeout=120) as r, open(local, "wb") as f:
                f.write(r.read())
        with lzma.open(local) as f:
            items = json.load(f)["cve_items"]
        for it in items:
            # il mirror da' l'oggetto CVE nudo, le API lo avvolgono in {"cve": ...}
            fns["update_db"](conn, {"cve": it})
        conn.commit()
        total += len(items)
        print(f"    {name:<22} {len(items):>7} CVE   (totale {total})")

    conn.commit()
    conn.close()
    tmp.rename(a.out)
    mb = a.out.stat().st_size / 1048576
    print(f"  fatto: {a.out}  {mb:.0f} MB, {total} CVE")


if __name__ == "__main__":
    main()
