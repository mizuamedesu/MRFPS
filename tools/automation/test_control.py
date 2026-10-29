import json
import argparse
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
import uuid
from unittest.mock import patch

from control import atomic_write, main, quest_submit, quest_result, submit


class ControlQueueTests(unittest.TestCase):
    def test_repeated_submission_reuses_identical_request_and_rejects_conflict(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            operation_id = str(uuid.uuid4())
            request = {'version': 1, 'id': operation_id, 'operation': 'view.set', 'parameters': {'view': 'free'}}
            result = submit(directory, request)
            self.assertEqual(result, submit(directory, request))
            file = directory / operation_id / 'request.json'
            self.assertEqual(json.loads(file.read_text()), request)
            with self.assertRaises(ValueError):
                submit(directory, {**request, 'parameters': {'view': 'player1'}})

    def test_atomic_publish_leaves_no_partial_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            path = directory / 'result.json'
            atomic_write(path, b'{"status":"ok"}')
            self.assertEqual(json.loads(path.read_text()), {'status': 'ok'})
            self.assertEqual([p.name for p in directory.iterdir()], ['result.json'])

    def test_quest_retries_match_existing_request_and_do_not_rewrite_it(self):
        operation_id = str(uuid.uuid4())
        request = {'version':1,'id':operation_id,'operation':'status','parameters':{}}
        args = argparse.Namespace(name='MRFPS-Client',operation_id=operation_id,serial='test-serial',adb='adb')
        with patch('control.adb',return_value=json.dumps(request).encode()) as mock_adb:
            result_path = quest_submit(args,request)
            self.assertTrue(result_path.endswith('/result.json'))
            self.assertEqual(mock_adb.call_count,1)
        with patch('control.adb',return_value=b'{"version":0}'):
            with self.assertRaises(ValueError): quest_submit(args,request)
        with patch('control.adb',side_effect=[None,b'',json.dumps(request).encode()]) as mock_adb:
            self.assertTrue(quest_submit(args,request).endswith('/result.json'))
            self.assertEqual(mock_adb.call_count,3)
        with patch('control.adb',return_value=None):
            self.assertIsNone(quest_result(args,result_path))

    def test_powershell_utf8_bom_parameter_file_and_same_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            params = root / 'parameters.json'
            params.write_text('{"view":"free"}',encoding='utf-8-sig')
            operation_id = str(uuid.uuid4())
            arguments = ['control.py','--name','MRFPS-Test','--operation-id',operation_id,
                         '--operation','view.set','--parameters-file',str(params),'--wait-seconds','0']
            with patch('control.root_directory',return_value=root), patch.object(sys,'argv',arguments), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(),2)
                self.assertEqual(main(),2)
                params.write_text('{"view":"player1"}',encoding='utf-8')
                with contextlib.redirect_stderr(io.StringIO()): self.assertEqual(main(),1)


if __name__ == '__main__':
    unittest.main()
