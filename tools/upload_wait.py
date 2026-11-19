# Vor dem Hochladen auf Enter warten – Zeit, das Panel anzustecken, den seriellen Monitor zu schliessen
# oder die Boot-Taste zu halten. Ohne Terminal (z. B. automatisierte Builds) oder mit
# PANEL_UPLOAD_NOWAIT=1 wird ohne Rueckfrage hochgeladen.
Import("env")  # noqa: F821 – stellt PlatformIO bereit
import os
import sys


def wait_enter(source, target, env):
    if os.environ.get("PANEL_UPLOAD_NOWAIT") or not sys.stdin or not sys.stdin.isatty():
        print("Upload ohne Rückfrage (kein Terminal oder PANEL_UPLOAD_NOWAIT gesetzt)")
        return
    port = env.subst("$UPLOAD_PORT") or "automatisch gesuchten Port"
    try:
        input("\n>>> Firmware ist gebaut. Bereit zum Hochladen auf %s – Enter drücken, Strg+C bricht ab ... " % port)
    except (EOFError, KeyboardInterrupt):
        print("\nHochladen abgebrochen.")
        env.Exit(1)


env.AddPreAction("upload", wait_enter)  # noqa: F821


# ---------------------------------------------------------------- Lokaler Release-Ordner
# Nach jedem erfolgreichen Build landen die fertigen Dateien in <repo>/release/lokal/ (nicht im Git) – je Umgebung
# immer nur die neueste, damit man zum Flashen nicht neu bauen muss:
#   <umgebung>.bin         Komplettdatei (Bootloader + Partitionen + App) – mit esptool ab 0x0 schreiben
#   <umgebung>-update.bin  nur die App – fuer Updates ueber die Weboberflaeche
#   <umgebung>@0x2F0000.bin  Updater (kleine Firmware im zweiten App-Bereich)
#   stand.txt              Version und Zeitpunkt je Umgebung
import shutil
import time


def _repo_root(env):
    d = env.subst("$PROJECT_DIR")
    for _ in range(4):
        if os.path.isfile(os.path.join(d, "tools", "upload_wait.py")):
            return d
        d = os.path.dirname(d)
    return env.subst("$PROJECT_DIR")


def _soc_version(root):
    try:
        for line in open(os.path.join(root, "src", "soc_version.h"), encoding="utf-8"):
            if "SOC_VERSION" in line and '"' in line:
                return line.split('"')[1]
    except OSError:
        pass
    return "?"


# Menuepunkt "Nur flashen (letzter Build)" (im PlatformIO-Menue der Umgebung unter "Custom"): spielt die zuletzt
# gebaute Firmware auf, ohne zu bauen. Schreibt nur die App (an ota_0, meist 0x10000) und boot_app0 (otadata -> startet
# wieder aus app0, auch nach einem Netz-Update). Bootloader, Partitionen und NVS bleiben – Einstellungen bleiben erhalten.
# Quelle: Build-Ordner; ist der leer (PlatformIO leert ihn manchmal), die Kopie in release/lokal/<umgebung>-update.bin.
def _app_offset(env):
    csv = env.subst("$PARTITIONS_TABLE_CSV")
    slot = env.GetProjectOption("custom_app_slot", "") or "ota_0"
    try:
        for line in open(csv, encoding="utf-8"):
            t = [x.strip() for x in line.split("#")[0].split(",")]
            if len(t) >= 4 and t[1] == "app" and t[2] in (slot, "factory" if slot == "ota_0" else slot):
                return int(t[3], 0)
    except (OSError, ValueError):
        pass
    return 0x10000


def flash_last(source, target, env):
    import subprocess
    name = env["PIOENV"]
    app = os.path.join(env.subst("$BUILD_DIR"), env.subst("${PROGNAME}.bin"))
    slot1 = env.GetProjectOption("custom_app_slot", "") == "ota_1"
    rel = os.path.join(_repo_root(env), "release", "lokal", name + ("@0x2F0000.bin" if slot1 else "-update.bin"))
    if not os.path.isfile(app):
        app = rel
    if not os.path.isfile(app):
        print("Nur flashen: keine gebaute Firmware gefunden (weder im Build-Ordner noch %s) – erst bauen." % rel)
        env.Exit(1)
    st = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(app)))
    off = _app_offset(env)
    images = []
    if not slot1:   # otadata zuruecksetzen: sonst startet ein Geraet nach einem Netz-Update weiter aus app1
        bapp = os.path.join(env.PioPlatform().get_package_dir("framework-arduinoespressif32") or "",
                            "tools", "partitions", "boot_app0.bin")
        if os.path.isfile(bapp):
            images += ["0xe000", bapp]
    images += [hex(off), app]
    if not env.subst("$UPLOAD_PORT"):
        env.AutodetectUploadPort()
    port = env.subst("$UPLOAD_PORT")
    uploader = env.subst("$UPLOADER").strip('"')
    cmd = [uploader, "--chip", env.BoardConfig().get("build.mcu", "esp32"), "--port", port,
           "--baud", env.subst("$UPLOAD_SPEED") or "460800", "--before", "default-reset", "--after", "hard-reset",
           "write-flash", "-z"] + images
    if uploader.lower().endswith(".py"):
        cmd = [env.subst("$PYTHONEXE")] + cmd
    print("Nur flashen: %s (Stand %s) nach %s an %s" % (os.path.relpath(app, _repo_root(env)), st, hex(off), port))
    rc = subprocess.call(cmd)
    if rc:
        env.Exit(rc)


env.AddCustomTarget(  # noqa: F821
    name="flash_last",
    dependencies=None,
    actions=[flash_last],
    title="Nur flashen (letzter Build)",
    description="Zuletzt gebaute Firmware aufspielen, ohne neu zu bauen (Einstellungen bleiben)",
)


def copy_release(source, target, env):
    name = env["PIOENV"]
    root = _repo_root(env)
    out = os.path.join(root, "release", "lokal")
    os.makedirs(out, exist_ok=True)
    bdir = env.subst("$BUILD_DIR")
    app = os.path.join(bdir, env.subst("${PROGNAME}.bin"))
    factory = os.path.join(bdir, env.subst("${PROGNAME}.factory.bin"))
    copied = []
    if env.GetProjectOption("custom_app_slot", "") == "ota_1":   # Updater: nur die App, feste Adresse
        dst = name + "@0x2F0000.bin"
        shutil.copyfile(app, os.path.join(out, dst))
        copied.append(dst)
    else:
        if os.path.isfile(factory):
            shutil.copyfile(factory, os.path.join(out, name + ".bin"))
            copied.append(name + ".bin")
        shutil.copyfile(app, os.path.join(out, name + "-update.bin"))
        copied.append(name + "-update.bin")
    # stand.txt: eine Zeile je Umgebung, die eigene ersetzen
    st = os.path.join(out, "stand.txt")
    lines = []
    if os.path.isfile(st):
        lines = [l for l in open(st, encoding="utf-8").read().splitlines() if l.strip() and not l.startswith(name + " ")]
    lines.append("%s  %s  %s  %s" % (name, _soc_version(root), time.strftime("%Y-%m-%d %H:%M"), ", ".join(copied)))
    open(st, "w", encoding="utf-8").write("\n".join(sorted(lines)) + "\n")
    print("Release-Ordner: %s -> release/lokal/" % ", ".join(copied))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_release)  # noqa: F821
