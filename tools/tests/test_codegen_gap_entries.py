"""Entry discovery for a tail thunk into code swallowed after a noreturn call."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('codegen_gaps', Path(__file__).parents[1] / 'codegen_gaps.py')
gaps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gaps)


class TailThunkEntries(unittest.TestCase):
    def setUp(self):
        self.base = gaps.BASE
        gaps.TEXT = (self.base, self.base + 0x80)
        gaps.IMAGE_RANGE = gaps.TEXT
        gaps.LABELS = set()
        words = [0] * 32
        words[0] = 0x48000028  # b alternate_entry
        words[8] = 0x38600000  # li r3,0: outer function
        words[9] = 0x48000041  # bl noreturn
        words[10] = 0x38600007  # li r3,7: alternate_entry
        words[11] = 0x4E800020  # blr
        gaps.data = struct.pack('>32I', *words)
        self.ends = {self.base: self.base + 4, self.base + 0x20: self.base + 0x30}

    def test_tail_thunk_declares_swallowed_entry(self):
        self.assertEqual(gaps.referenced_entries(sorted(self.ends), self.ends),
                         {self.base + 0x28: 'tail thunk'})

    def test_existing_internal_label_is_not_promoted(self):
        gaps.LABELS.add(self.base + 0x28)
        self.assertEqual(gaps.referenced_entries(sorted(self.ends), self.ends), {})

    def test_larger_function_branch_is_not_a_thunk(self):
        self.ends[self.base] = self.base + 8
        self.assertEqual(gaps.referenced_entries(sorted(self.ends), self.ends), {})

    def test_linked_branch_is_not_a_tail_thunk(self):
        gaps.data = struct.pack('>I', 0x48000029) + gaps.data[4:]
        self.assertEqual(gaps.referenced_entries(sorted(self.ends), self.ends), {})


if __name__ == '__main__':
    unittest.main()
