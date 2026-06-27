from pathlib import Path
import re

Import("env")


def patch_lilygo_epd_driver():
    frame_count = int(env.GetProjectOption("custom_lilygo_epd_frame_count", "8"))
    frame_count = max(1, min(frame_count, 15))

    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    driver_files = list(libdeps_dir.glob("*/LilyGo-EPD47/src/epd_driver.c"))

    if not driver_files:
        print("LilyGo EPD patch: epd_driver.c ainda nao encontrado.")
        return

    driver_path = driver_files[0]
    source = driver_path.read_text()

    pattern = r"    uint8_t frame_count = ([0-9]+);"
    patched = f"    uint8_t frame_count = {frame_count};"

    if patched in source:
        print(f"LilyGo EPD patch: frame_count ja esta em {frame_count}.")
        return

    match = re.search(pattern, source)
    if not match:
        print("LilyGo EPD patch: padrao frame_count nao encontrado; mantendo biblioteca sem alteracao.")
        return

    old_frame_count = match.group(1)
    driver_path.write_text(re.sub(pattern, patched, source, count=1))
    print(f"LilyGo EPD patch: epd_draw_image frame_count {old_frame_count} -> {frame_count}.")


patch_lilygo_epd_driver()
