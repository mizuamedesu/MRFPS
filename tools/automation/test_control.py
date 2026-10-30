import json
import argparse
import contextlib
import concurrent.futures
import io
import pathlib
import sys
import tempfile
import threading
import unittest
import uuid
from unittest.mock import patch

from control import archive_local_locked, atomic_write, encode, lock_directory, main, quest_submit, quest_result, submit


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

    def test_desktop_input_retry_preserves_the_original_lease_request(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            operation_id = str(uuid.uuid4())
            parameters = {'fire':True, 'shield':False, 'move':[1,0],
                          'look_degrees_per_second':[0,0], 'duration_seconds':0.5}
            request = {'version':1, 'id':operation_id, 'operation':'player.input.set', 'parameters':parameters}
            result = submit(directory, request)
            atomic_write(result, encode({'id':operation_id,'status':'ok','code':'INPUT_LEASE_SET'}))
            self.assertEqual(submit(directory, request), result)
            self.assertEqual(json.loads(result.read_text())['status'], 'ok')
            with self.assertRaises(ValueError):
                submit(directory, {**request, 'parameters':{**parameters, 'duration_seconds':1.0}})
            with lock_directory(directory):
                self.assertEqual(archive_local_locked(directory,10)['archived'],1)
            self.assertEqual(submit(directory,request),directory/'archive'/operation_id/'result.json')

    def test_capture_stop_replay_preserves_submitted_receipt_and_rejects_enable(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=pathlib.Path(temporary); operation_id=str(uuid.uuid4())
            request={'version':1,'id':operation_id,'operation':'capture.set','parameters':{'enabled':False}}
            result=submit(directory,request)
            started=directory/operation_id/'started'; started.write_text('started',encoding='utf-8')
            receipt={'id':operation_id,'status':'submitted','code':'CAPTURE_STOP_SUBMITTED'}
            atomic_write(result,encode(receipt))
            self.assertEqual(submit(directory,request),result)
            self.assertEqual(json.loads(result.read_text()),receipt)
            self.assertTrue(started.is_file())
            with self.assertRaises(ValueError):
                submit(directory,{**request,'parameters':{'enabled':True}})
            with lock_directory(directory):
                self.assertEqual(archive_local_locked(directory,10)['archived'],0)
            self.assertEqual(submit(directory,request),result)

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
        with patch('control.adb',return_value=b'active') as mock_adb:
            result_path = quest_submit(args,request)
            self.assertTrue(result_path.endswith('/result.json'))
            self.assertEqual(mock_adb.call_count,1)
            self.assertIn('.lock-'+operation_id,mock_adb.call_args.args[1][-1])
        with patch('control.adb',return_value=b'conflict'):
            with self.assertRaises(ValueError): quest_submit(args,request)
        with patch('control.adb',return_value=b'archived'):
            self.assertIn('/archive/',quest_submit(args,request))
        with patch('control.adb',return_value=None):
            self.assertIsNone(quest_result(args,result_path))

    def test_archive_preserves_terminal_receipts_and_skips_uncertain(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=pathlib.Path(temporary)
            ids=[str(uuid.uuid4()) for _ in range(4)]
            requests=[{'version':1,'id':item,'operation':'status','parameters':{}} for item in ids]
            for request in requests: submit(directory,request)
            for request,state in zip(requests,['ok','error','unknown','submitted']):
                atomic_write(directory/request['id']/'result.json',encode({'id':request['id'],'status':state}))
            with lock_directory(directory): result=archive_local_locked(directory,100)
            self.assertEqual(result['archived'],2)
            for request in requests[:2]:
                self.assertEqual(submit(directory,request),directory/'archive'/request['id']/'result.json')
                with self.assertRaises(ValueError): submit(directory,{**request,'operation':'view.set'})
            for request in requests[2:]: self.assertTrue((directory/request['id']).is_dir())
            with lock_directory(directory): self.assertEqual(archive_local_locked(directory,100)['archived'],0)

    def test_concurrent_submit_and_archive_never_create_duplicate_operation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=pathlib.Path(temporary)
            operation_id=str(uuid.uuid4())
            request={'version':1,'id':operation_id,'operation':'status','parameters':{}}
            submit(directory,request)
            atomic_write(directory/operation_id/'result.json',encode({'id':operation_id,'status':'ok'}))
            barrier=threading.Barrier(2)
            def archive():
                barrier.wait()
                with lock_directory(directory): return archive_local_locked(directory,100)
            def retry():
                barrier.wait()
                return submit(directory,request)
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                a=pool.submit(archive); b=pool.submit(retry)
                a.result(); b.result()
            self.assertFalse((directory/operation_id).exists())
            self.assertTrue((directory/'archive'/operation_id/'result.json').exists())

    def test_archive_cli_repeat_and_archived_result_retry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=pathlib.Path(temporary); directory=root/'MRFPS-Test'; directory.mkdir()
            operation_id=str(uuid.uuid4())
            request={'version':1,'id':operation_id,'operation':'status','parameters':{}}
            submit(directory,request)
            atomic_write(directory/operation_id/'result.json',encode({'id':operation_id,'status':'ok'}))
            archive_args=['control.py','--name','MRFPS-Test','--archive-completed']
            with patch('control.root_directory',return_value=root), patch.object(sys,'argv',archive_args):
                first=io.StringIO()
                with contextlib.redirect_stdout(first): self.assertEqual(main(),0)
                second=io.StringIO()
                with contextlib.redirect_stdout(second): self.assertEqual(main(),0)
                self.assertEqual(json.loads(first.getvalue())['archived'],1)
                self.assertEqual(json.loads(second.getvalue())['archived'],0)
            retry_args=['control.py','--name','MRFPS-Test','--operation-id',operation_id,'--operation','status','--wait-seconds','0']
            with patch('control.root_directory',return_value=root), patch.object(sys,'argv',retry_args), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(),0)

    def test_quest_archive_cli_routes_to_device_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=pathlib.Path(temporary)
            arguments=['control.py','--platform','quest','--serial','fixture-serial','--archive-completed']
            with patch('control.root_directory',return_value=root), patch.object(sys,'argv',arguments), \
                 patch('control.quest_archive_locked',return_value={'archived':0,'skipped':0,'remote_archived':0}) as archive, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(),0)
                self.assertEqual(archive.call_count,1)

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
