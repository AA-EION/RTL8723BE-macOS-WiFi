import importlib.util
import unittest

spec = importlib.util.spec_from_file_location('test_boot', 'scripts/configure_test_boot.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class TestBootConfig(unittest.TestCase):
    def test_prepare_disable_idempotence_and_unrelated_settings(self):
        original = {'Kernel': {'Add': [
            {'BundlePath': 'Lilu.kext', 'Enabled': True},
            {'BundlePath': 'RTL8723BEWiFi.kext', 'Enabled': True}],
            'Force': [{'BundlePath': 'RTL8723BEWiFi.kext', 'Enabled': True}]},
            'NVRAM': {'Add': {mod.APPLE_GUID: {'boot-args': 'debug=0x100 keepsyms=1'}}},
            'PlatformInfo': {'Generic': {'SystemProductName': 'unchanged'}}}
        prepared = mod.configure(original, True)
        self.assertTrue(original['Kernel']['Add'][1]['Enabled'])
        self.assertFalse(prepared['Kernel']['Add'][1]['Enabled'])
        self.assertFalse(prepared['Kernel']['Force'][0]['Enabled'])
        self.assertEqual(prepared['Kernel']['Add'][0], original['Kernel']['Add'][0])
        self.assertEqual(prepared['PlatformInfo'], original['PlatformInfo'])
        self.assertEqual(mod.configure(prepared, True), prepared)
        disabled = mod.configure(prepared, False)
        self.assertEqual(disabled['NVRAM'], original['NVRAM'])
        self.assertFalse(disabled['Kernel']['Add'][1]['Enabled'])

    def test_reject_unrelated_config(self):
        with self.assertRaises(ValueError):
            mod.configure({'Kernel': {'Add': []}}, True)


if __name__ == '__main__':
    unittest.main()
