#!/usr/bin/env python3
"""Fetch pinned esp-sdr sources and generate the standalone S3 experiment."""

import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parent
WORK = ROOT / ".work"


def fetch(name, source):
    revision = source["commit"]
    destination = WORK / "vendor" / f"{name}-{revision}"
    archive = WORK / "downloads" / f"{name}-{revision}.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        url = f"https://codeload.github.com/{source['repository']}/tar.gz/{revision}"
        print(f"Downloading {source['repository']} at {revision}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as response:
            with archive.with_suffix(".part").open("wb") as output:
                shutil.copyfileobj(response, output)
        archive.with_suffix(".part").replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=WORK) as temporary:
            with tarfile.open(archive) as bundle:
                bundle.extractall(temporary, filter="data")
            directories = list(Path(temporary).iterdir())
            if len(directories) != 1 or not directories[0].is_dir():
                raise RuntimeError(f"Unexpected archive layout: {archive}")
            directories[0].rename(destination)
    return destination, digest


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Upstream integration anchor changed: {old!r}")
    return text.replace(old, new, 1)


def main():
    WORK.mkdir(exist_ok=True)
    lock = json.loads((ROOT / "upstream.json").read_text())
    sources, hashes = {}, {}
    for name in ["esp-sdr", "esp-dsp"]:
        sources[name], hashes[name] = fetch(name, lock[name])
    firmware = WORK / "firmware"
    shutil.copytree(sources["esp-sdr"], firmware, dirs_exist_ok=True)
    shutil.copytree(sources["esp-dsp"], firmware / "components" / "esp-dsp", dirs_exist_ok=True)
    experiment = firmware / "main" / "experiment"
    experiment.mkdir(exist_ok=True)
    for name in ["demod.c", "demod.h"]:
        shutil.copy2(ROOT / name, experiment / name)
    shutil.copy2(ROOT / "firmware" / "halo_experiment.inc", experiment)
    receiver = firmware / "main" / "targets" / "esp32s3" / "receiver.c"
    text = receiver.read_text()
    text = replace_once(text, "static void handle_command(char *line) {",
                        '#include "halo_experiment.inc"\n\nstatic void handle_command(char *line) {\n'
                        '    if (halo_command(line)) return;')
    text = replace_once(text, "void app_main(void) {",
                        'void app_main(void) {\n    halo_board_init();\n    puts("HALOBOOT reset held");')
    text = replace_once(text, "    ESP_ERROR_CHECK(esp_wifi_start());",
                        '    ESP_ERROR_CHECK(esp_wifi_start());\n    puts("HALOBOOT PHY started");')
    text = replace_once(text, "    prepare_rx();\n    esp_log_level_set",
                        '    prepare_rx();\n    puts("HALOBOOT receiver tuned");\n    esp_log_level_set')
    text = replace_once(text, "    burst_serial_init();\n    ring_capture_init();",
                        '    burst_serial_init();\n    reply("HALOBOOT serial ready\\n");\n'
                        '    ring_capture_init();\n    reply("HALOBOOT capture ready\\n");')
    receiver.write_text(text)
    defaults = firmware / "sdkconfig.defaults.esp32s3"
    defaults.write_text(defaults.read_text().replace("CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y",
                                                     "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y")
                        + "\nCONFIG_SPIRAM=y\nCONFIG_SPIRAM_MODE_QUAD=y\n"
                          "CONFIG_SPIRAM_SPEED_40M=y\nCONFIG_SPIRAM_USE_MALLOC=y\n")
    # Our packet results use the normal serial transport after capture. Keep
    # a smaller upstream spectrum queue to leave internal RAM for this SDK's
    # PSRAM support; this does not change RF bank sizes or ownership checks.
    ring = firmware / "main" / "common" / "ring_capture.c"
    ring.write_text(replace_once(ring.read_text(),
                                "#define TXQ_SIZE 16384u /* 7 frames of 2048 bins, 56 of 256 */",
                                "#define TXQ_SIZE 8192u /* Experiment: bounded spectrum output queue. */"))
    cmake = firmware / "main" / "CMakeLists.txt"
    cmake.write_text(replace_once(cmake.read_text(), "set(dependencies esp_phy", "set(dependencies esp_driver_gpio esp_phy")
                     + '\ntarget_sources(${COMPONENT_LIB} PRIVATE "experiment/demod.c")\n'
                       'target_include_directories(${COMPONENT_LIB} PRIVATE "experiment")\n')
    experiment_digest = hashlib.sha256()
    for path in [ROOT / "demod.c", ROOT / "demod.h", ROOT / "firmware" / "halo_experiment.inc",
                 ROOT / "prepare.py", ROOT / "upstream.json"]:
        experiment_digest.update(path.read_bytes())
    (experiment / "halo_version.h").write_text(f'#define HALO_BUILD_ID "{experiment_digest.hexdigest()[:12]}"\n')
    provenance = {"sources": lock, "archive_sha256": hashes}
    provenance["experiment_sha256"] = experiment_digest.hexdigest()
    (firmware / "experiment-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"Prepared {firmware}")


if __name__ == "__main__":
    main()
