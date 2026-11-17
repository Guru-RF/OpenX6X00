#!/usr/bin/env python3
"""What x6100-update tells the GUI and the web Update page on a multiboot card (rootfs/usr/sbin/x6100-update): the
updater's own code, its files in a folder of its own under build/tmp, GitHub stood in for. See the Makefile:

    make          (python3 -I -B update_test.py [-v])

1. status: "image" has "multiboot", how S01multiboot was told and the app's page, on a multiboot card; false, "" and
   none on a card of its own. Without S01multiboot's mark the menu's x6100mb= on the kernel's command line still
   tells it.
2. check: a newer release that wants a newer card image says so as before, and "multiboot" with the app's page on a
   multiboot card (the GUI reads "multiboot": true from the line); false on a card of its own.
"""
import io
import json
import os
import runpy
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
UPDATER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-update")
OWN = "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait panic=10\n"


class Response(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class Multiboot(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        d = self.dir.name
        self.up = runpy.run_path(UPDATER)["image_state"].__globals__
        self.mark, self.cmdline = os.path.join(d, "x6100-multiboot"), os.path.join(d, "cmdline")
        self.version, stamp, required = (os.path.join(d, n) for n in ("app-version", "stamp", "image-required"))
        with open(self.version, "w") as f:
            f.write("0.3.12\n")
        with open(stamp, "w") as f:
            f.write("level=2\n")
        with open(required, "w") as f:
            f.write("x6100 3 the WiFi driver fix\nx6200 3 the WiFi driver fix\n")
        self.cmd(OWN)
        self.up.update(MULTIBOOT=self.mark, CMDLINE=self.cmdline, VERSION_FILE=self.version, IMAGE_STAMP=stamp,
                       IMAGE_REQUIRED=required, PROGRESS=os.path.join(d, "progress"),
                       PENDING=os.path.join(d, "pending.json"), LAST=os.path.join(d, "last.json"),
                       radio_model=lambda: "x6100", http=self.github)

    def tearDown(self):
        self.dir.cleanup()

    def cmd(self, text):
        with open(self.cmdline, "w") as f:
            f.write(text)

    def marked(self, how):
        with open(self.mark, "w") as f:
            f.write(how + "\n")

    def github(self, url, timeout=None):
        """The latest release, v0.3.13 with its bundles and card images, and its image-required (level 4)"""
        if url == self.up["API"]:
            assets = [{"name": f"OpenX6X00-{m}-v0.3.13-{k}", "browser_download_url": f"https://example.invalid/{m}-{k}"}
                      for m in ("x6100", "x6200") for k in ("update.tar.gz", "sdcard.img.zip")]
            return Response(json.dumps({"tag_name": "v0.3.13", "assets": assets, "body": "notes",
                                        "html_url": "https://example.invalid/release"}).encode())
        if url == self.up["RAW"].format(tag="0.3.13"):
            return Response(b"x6100 4 a newer kernel\nx6200 4 a newer kernel\n")
        raise OSError(f"no such page here: {url}")

    def test_status_own_card(self):
        img = self.up["cmd_status"]([])["image"]
        self.assertEqual((img["needed"], img["note"]), (True, "the WiFi driver fix"))
        self.assertEqual((img["multiboot"], img["multiboot_how"], img["multiboot_app"]), (False, "", None))

    def test_status_multiboot(self):
        self.marked("root on partition 5")
        img = self.up["cmd_status"]([])["image"]
        self.assertEqual((img["needed"], img["multiboot"], img["multiboot_how"]), (True, True, "root on partition 5"))
        self.assertEqual(img["multiboot_app"], self.up["MULTIBOOT_APP"])
        self.assertTrue(img["multiboot_app"].startswith("https://github.com/"))

    def test_status_menu_mark_only(self):
        """S01multiboot did not run: the menu's mark on the command line, as a whole argument"""
        self.cmd("console=ttyS0,115200 root=/dev/mmcblk0p6 rootwait x6100mb=3\n")
        self.assertEqual(self.up["cmd_status"]([])["image"]["multiboot_how"], "boot menu")
        self.cmd("x6100mb=1 root=/dev/mmcblk0p2\n")
        self.assertTrue(self.up["cmd_status"]([])["image"]["multiboot"])
        self.cmd("root=/dev/mmcblk0p2 nox6100mb=1\n")
        self.assertFalse(self.up["cmd_status"]([])["image"]["multiboot"])
        os.remove(self.cmdline)
        self.assertFalse(self.up["cmd_status"]([])["image"]["multiboot"])

    def test_check_own_card(self):
        out = self.up["cmd_check"]([])
        self.assertEqual((out["newer"], out["image_needed"], out["image_note"]), (True, True, "a newer kernel"))
        self.assertEqual(out["image_url"], "https://example.invalid/x6100-sdcard.img.zip")
        self.assertEqual((out["multiboot"], out["multiboot_app"]), (False, None))

    def test_check_multiboot(self):
        self.marked("boot menu on the card (bootmenu.env)")
        out = self.up["cmd_check"]([])
        self.assertEqual((out["newer"], out["image_needed"], out["multiboot"]), (True, True, True))
        self.assertEqual(out["multiboot_app"], self.up["MULTIBOOT_APP"])
        # the GUI reads the line as the updater prints it (src/settings_window.cpp, upd_check_done)
        line = json.dumps(out)
        self.assertIn('"image_needed": true', line)
        self.assertIn('"multiboot": true', line)


if __name__ == "__main__":
    unittest.main()
