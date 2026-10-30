import hashlib
import json
import unittest
from unittest import mock

import public_ghcr as g


class PublicRegistryTests(unittest.TestCase):
    def setUp(self):
        self.bundle, self.server, self.commit = 'a'*64, 'b'*64, 'c'*40
        self.package, self.tag = g.validate(self.bundle, self.server, self.commit)
        self.metadata = {'name': self.package, 'package_type':'container', 'visibility':'public',
                         'owner':{'login':g.OWNER}, 'repository':{'full_name':g.REPOSITORY}}
        labels = {'org.opencontainers.image.source':'https://github.com/'+g.REPOSITORY,
                  'org.opencontainers.image.revision':self.commit,
                  'app.mrfps.bundle.sha256':self.bundle, 'app.mrfps.server.sha256':self.server}
        self.config = json.dumps({'config':{'Labels':labels}}).encode()
        self.config_sha = 'sha256:'+hashlib.sha256(self.config).hexdigest()
        self.layer = 'sha256:'+'d'*64
        self.manifest = json.dumps({'config':{'digest':self.config_sha},
                                    'layers':[{'digest':self.layer,'size':123}]}).encode()
        self.digest = 'sha256:'+hashlib.sha256(self.manifest).hexdigest()

    def request(self, url, authorization=None, **kwargs):
        if url.startswith('https://api.github.com/'):
            return 200, json.dumps(self.metadata).encode(), {}
        if '/tags/list' in url:
            return 200, json.dumps({'name':g.OWNER+'/'+self.package,'tags':[self.tag]}).encode(), {}
        if '/manifests/' in url:
            return 200, self.manifest, {'docker-content-digest':self.digest}
        if self.config_sha in url:
            return 200, self.config, {}
        if self.layer in url:
            return 206, b'x', {}
        raise AssertionError(url)

    def test_fresh_package_has_distinct_digest_derived_name(self):
        with mock.patch.object(g.registry,'token_for',return_value=(200,'Bearer redacted')), \
             mock.patch.object(g.registry,'request',return_value=(404,b'',{})):
            self.assertEqual(g.preflight(self.bundle,self.server,self.commit,'actor','token')['status'],'absent')
        self.assertNotEqual(self.package,'mrfps-server-private')

    def test_existing_package_requires_anonymous_access(self):
        with mock.patch.object(g.registry,'token_for',return_value=(200,'Bearer redacted')), \
             mock.patch.object(g.registry,'request',side_effect=self.request):
            result=g.preflight(self.bundle,self.server,self.commit,'actor','token')
        self.assertEqual(result['status'],'public')
        self.assertEqual(result['image'],f'ghcr.io/{g.OWNER}/{self.package}@{self.digest}')
        self.assertTrue(result['anonymous_manifest_and_layer_verified'])

    def test_private_or_inaccessible_package_fails_closed(self):
        with mock.patch.object(g.registry,'token_for',return_value=(200,'Bearer redacted')), \
             mock.patch.object(g.registry,'request',return_value=(403,b'',{})):
            with self.assertRaisesRegex(ValueError,'STATE_UNVERIFIED'):
                g.preflight(self.bundle,self.server,self.commit,'actor','token')

    def test_wrong_package_owner_or_labels_are_rejected(self):
        self.metadata['visibility']='private'
        with mock.patch.object(g.registry,'token_for',return_value=(200,'Bearer redacted')), \
             mock.patch.object(g.registry,'request',side_effect=self.request):
            with self.assertRaisesRegex(ValueError,'VISIBILITY_MISMATCH'):
                g.verify_public(self.bundle,self.server,self.commit,'token')
        self.metadata['visibility']='public'
        bad_server='e'*64
        with mock.patch.object(g.registry,'token_for',return_value=(200,'Bearer redacted')), \
             mock.patch.object(g.registry,'request',side_effect=self.request):
            with self.assertRaisesRegex(ValueError,'LABEL_MISMATCH'):
                g.verify_public(self.bundle,bad_server,self.commit,'token')

    def test_manifest_digest_and_anonymous_layer_checked(self):
        def bad_layer(url,*args,**kwargs):
            if self.layer in url: return 403,b'',{}
            return self.request(url,*args,**kwargs)
        with mock.patch.object(g.registry,'token_for',return_value=(200,'Bearer redacted')), \
             mock.patch.object(g.registry,'request',side_effect=bad_layer):
            with self.assertRaisesRegex(ValueError,'ANONYMOUS_LAYER_HTTP_403'):
                g.verify_public(self.bundle,self.server,self.commit,'token')


if __name__ == '__main__': unittest.main()