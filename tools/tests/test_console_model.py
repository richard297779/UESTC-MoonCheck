import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from console_model import patch_payload, resolve_account, selected_payload, notification_labels


class AccountCommands(unittest.TestCase):
    def setUp(self):
        self.a = dict(slot=0, label='A', user='fixture-01', enabled=False)
        self.b = dict(slot=1, label='B', user='fixture-02', enabled=True)
        self.snapshot = dict(revision=27, accounts=[self.a, self.b])

    def test_patch_omits_empty_fields_and_preserves_address_spaces(self):
        result = patch_payload(self.snapshot, dict(label='', user='', bfp='', **{'pass': '', 'addr': '  fixture address  '}), self.a)
        self.assertEqual(result, dict(op='account.patch', create=False, expectedRevision=27, target='A', fields={'addr': '  fixture address  '}))

    def test_empty_or_incomplete_create_is_rejected(self):
        for values in ({}, dict(label='C', user='fixture', **{'pass': 'fixture'})):
            with self.assertRaises(ValueError): patch_payload(self.snapshot, values, creating=True)

    def test_coordinate_pair_and_fingerprint_validation(self):
        for values in (dict(lat='30'), dict(lat='nan', lng='104'), dict(lat='0', lng='104'), dict(bfp='bad')):
            with self.assertRaises(ValueError): patch_payload(self.snapshot, values, self.a)
        self.assertEqual(patch_payload(self.snapshot, dict(lat='30.1', lng='104.2'), self.a)['fields'], dict(lat=30.1, lng=104.2))

    def test_explicit_lookup_and_duplicate_student(self):
        self.assertIs(resolve_account([self.a,self.b], '1', 'slot'), self.b)
        self.assertIs(resolve_account([self.a,self.b], 'A', 'label'), self.a)
        with self.assertRaises(ValueError): resolve_account([self.a,dict(self.b,user=self.a['user'])], self.a['user'], 'user')

    def test_selection_rejects_disabled_and_duplicates(self):
        for rows in ([], [self.a], [self.b,self.b]):
            with self.assertRaises(ValueError): selected_payload(self.snapshot, rows, 'test.login')
        self.assertEqual(selected_payload(self.snapshot,[self.b],'test.login')['targets'],['B'])

    def test_notification_links_include_disabled_channels(self):
        result=notification_labels(dict(on=True,channels=[dict(label='all-users',accounts='all',on=True),dict(label='A-only',accounts='A',on=False),dict(label='B-only',accounts='B',on=True)]),'A')
        self.assertIn('A-only（停用）',result)
        self.assertIn('all-users',result)
        self.assertNotIn('B-only',result)


if __name__ == '__main__': unittest.main()
