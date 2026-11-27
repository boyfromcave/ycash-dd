#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""ycash-dd only: the release guard accepts the 6.23.x pairing tag of ycash6's Yugen line (ruling 18).

qa/test_yellowback_release_heights.py is byte-identical on both node lines (the cross-line
manifest), and ycash6's vault line refuses 6.23.x, so this ycash-dd-only case lives here.
No tags are pushed.
"""

import os
import shutil
import tempfile
import unittest

import test_yellowback_release_heights as rh


def write_notes(root, version):
    path = os.path.join(root, "doc/release-notes/release-notes-%s.md" % version)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("ycashd v%s\n\nThe changes.\n" % version)


class YugenPairing(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="yb-release-pairing-")
        self.addCleanup(shutil.rmtree, self.tmp)

    def test_6_23_pairing_tag_is_accepted_with_the_6_22_rules(self):
        root = os.path.join(self.tmp, "ready")
        rh.write_tree(root)
        for version in ("6.23.0-beta4", "6.23.0-rc1", "6.23.0", "6.22.0-beta4"):
            write_notes(root, version)
        for version, stage in (("6.23.0-beta4", "beta"), ("6.23.0-rc1", "rc"), ("6.23.0", "final")):
            code, out = rh.run(["--root", root, "--release-version", version])
            self.assertIn("release stage: " + stage, out)
            # The same rules as the 6.22 tag of the same stage.
            ref_code, _ = rh.run(["--root", root, "--release-version", version.replace("6.23", "6.22", 1)])
            self.assertEqual(code, ref_code, out)

    def test_6_23_beta_needs_its_release_notes(self):
        root = os.path.join(self.tmp, "nonotes")
        rh.write_tree(root)
        code, out = rh.run(["--root", root, "--release-version", "6.23.0-beta4"])
        self.assertEqual(code, 1, out)
        self.assertIn("R10: no release notes for 6.23.0-beta4", out)
        write_notes(root, "6.23.0-beta4")
        code, out = rh.run(["--root", root, "--release-version", "6.23.0-beta4"])
        self.assertEqual(code, 0, out)

    def test_other_lines_are_still_refused(self):
        for version in ("6.21.4-rc1", "6.24.0-beta1"):
            code, out = rh.run(["--warn", "--release-version", version])
            self.assertEqual(code, 1, out)
            self.assertIn("is not 6.22.x (the vault release line) or 6.23.x", out)


if __name__ == "__main__":
    unittest.main()
