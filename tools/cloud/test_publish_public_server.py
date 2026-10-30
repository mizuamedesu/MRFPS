import json
import unittest
from unittest import mock

import publish_public_server as publish


class PublicPublisherTests(unittest.TestCase):
    def setUp(self):
        self.bundle,self.server,self.source='a'*64,'b'*64,'c'*40
        self.tag='server-artifacts-'+self.bundle[:12]+'-'+self.source[:12]

    def test_draft_requires_exact_existing_digest(self):
        self.assertFalse(publish.validate_asset({'assets':[]},self.bundle))
        asset={'name':'linux-server.tar.gz','digest':'sha256:'+self.bundle,'state':'uploaded'}
        self.assertTrue(publish.validate_asset({'assets':[asset]},self.bundle))
        asset['digest']='sha256:'+'f'*64
        with self.assertRaisesRegex(RuntimeError,'DIGEST_MISMATCH'): publish.validate_asset({'assets':[asset]},self.bundle)

    def test_public_or_ambiguous_release_is_never_reused(self):
        with mock.patch.object(publish,'releases',return_value=[{'tag_name':self.tag,'draft':False}]):
            with self.assertRaisesRegex(RuntimeError,'MUST_REMAIN_DRAFT'): publish.find_draft(self.tag)
        with mock.patch.object(publish,'releases',return_value=[{'tag_name':self.tag,'draft':True}]*2):
            with self.assertRaisesRegex(RuntimeError,'AMBIGUOUS'): publish.find_draft(self.tag)

    def test_dispatch_is_idempotent_for_active_or_verified_run(self):
        workflow={'state':'active','path':'.github/workflows/'+publish.WORKFLOW}
        title=f'Public server {self.bundle} {self.source}'
        for status,conclusion,expected in [('in_progress',None,'already_in_progress'),('completed','success','already_verified')]:
            runs=[{'displayTitle':title,'databaseId':123,'status':status,'conclusion':conclusion}]
            with self.subTest(status=status),mock.patch.object(publish,'api',return_value=workflow), \
                 mock.patch.object(publish,'gh',return_value=json.dumps(runs)) as gh:
                self.assertEqual(publish.dispatch(self.tag,self.source,self.bundle,self.server)['status'],expected)
                self.assertEqual(gh.call_count,1)

    def test_failed_run_can_be_retried(self):
        workflow={'state':'active','path':'.github/workflows/'+publish.WORKFLOW}
        title=f'Public server {self.bundle} {self.source}'
        runs=[{'displayTitle':title,'databaseId':123,'status':'completed','conclusion':'failure'}]
        with mock.patch.object(publish,'api',return_value=workflow), \
             mock.patch.object(publish,'gh',side_effect=[json.dumps(runs),'']) as gh:
            self.assertEqual(publish.dispatch(self.tag,self.source,self.bundle,self.server)['status'],'dispatched')
            self.assertEqual(gh.call_count,2)


if __name__ == '__main__': unittest.main()