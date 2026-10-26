"""Behavioral characterization for the public byte-size wrappers."""

import unittest

from scripts import config, experiment


class ByteSizeWrapperTests(unittest.TestCase):
    def test_config_wrapper_characterization(self):
        cases = [
            ("32KB", 32768),
            (" 32 kb ", 32768),
            ("1MB", 1048576),
            ("42", 42),
            (42, 42),
            (True, "TRUE"),
            (None, "NONE"),
            ("1.5KB", 15360),
            ("-1KB", 1024),
            ("1e3KB", 13312),
            ("1GB", 1),
            ("0x10", "0X10"),
            ("1_024", 1024),
            ("not-a-size", "NOT-A-SIZE"),
            ("0B", 0),
            ("1 B", 1),
        ]
        for value, expected in cases:
            with self.subTest(value=value):
                result = config.parse_byte_size(value)
                self.assertEqual(result, expected)
                self.assertIs(type(result), type(expected))

    def test_experiment_wrapper_characterization(self):
        cases = [
            ("32KB", 32768),
            (" 32 kb ", 32768),
            ("1MB", 1048576),
            ("42", 42),
            (42, 42),
            (True, ValueError),
            (None, ValueError),
            ("1.5KB", 15360),
            ("-1KB", 1024),
            ("1e3KB", 13312),
            ("1GB", 1),
            ("0x10", ValueError),
            ("1_024", 1024),
            ("not-a-size", ValueError),
            ("0B", 0),
            ("1 B", 1),
        ]
        for value, expected in cases:
            with self.subTest(value=value):
                if isinstance(expected, type) and issubclass(expected, Exception):
                    with self.assertRaises(expected):
                        experiment.parse_byte_size(value)
                else:
                    result = experiment.parse_byte_size(value)
                    self.assertEqual(result, expected)
                    self.assertIs(type(result), type(expected))

    def test_empty_kilobyte_suffix_raises_in_both_wrappers(self):
        for parse in (config.parse_byte_size, experiment.parse_byte_size):
            with self.subTest(wrapper=parse.__module__):
                with self.assertRaises(ValueError):
                    parse("KB")


if __name__ == "__main__":
    unittest.main()
