"""Run the deploy script against a fixture adb executable, never a device."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT=Path(__file__).resolve().parents[2]
POWERSHELL=Path(os.environ.get("SystemRoot", "C:/Windows"))/"System32/WindowsPowerShell/v1.0/powershell.exe"
EMPTY="  * Task{abcd #100 type=home sz=1}\n"
PROMPT="  * Task{abcd #484 type=standard sz=1}\n    * Hist  #0: ActivityRecord{dead u0 com.meta.systemui/.media.MediaProjectionPermissionActivity t484}\n"


@unittest.skipUnless(os.name=="nt", "fixture executable and deploy script require Windows")
class QuestDeployPermissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix="mrfps-fake-adb-")
        cls.root=Path(cls.temp.name)
        cls.sdk=cls.root/"sdk"; (cls.sdk/"platform-tools").mkdir(parents=True)
        cls.adb=cls.sdk/"platform-tools"/"adb.exe"
        source=cls.root/"fixture.cs"
        source.write_text(r'''using System;
using System.IO;
class Fixture {
    static int Main(string[] a) {
        string log=Environment.GetEnvironmentVariable("MRFPS_TEST_ADB_LOG");
        int dumps=0;
        if (File.Exists(log)) foreach (string line in File.ReadAllLines(log)) if (line.Contains(" shell dumpsys activity activities")) ++dumps;
        File.AppendAllText(log,String.Join(" ",a)+"\n");
        if (a.Length>0 && a[0]=="devices") { Console.WriteLine("List of devices attached\nfixture device product:fake"); return 0; }
        if (a.Length>=5 && a[2]=="shell" && a[3]=="dumpsys" && a[4]=="activity") {
            string path=Environment.GetEnvironmentVariable("MRFPS_TEST_ADB_DUMP"+(dumps+1));
            Console.Write(File.ReadAllText(path)); return 0;
        }
        if (a.Length>=5 && a[2]=="shell" && a[3]=="pidof") return 1;
        if (a.Length>=6 && a[2]=="shell" && a[3]=="run-as" && a[5]=="pwd") {
            Console.WriteLine("/data/user/0/"+a[4]); return 0;
        }
        if (a.Length>=5 && a[2]=="shell" && a[3]=="dumpsys" && a[4]=="power") {
            Console.WriteLine("mWakefulness=Awake"); return 0;
        }
        return 0;
    }
}''',encoding="utf-8")
        env={**os.environ,"MRFPS_TEST_ADB_SOURCE":str(source),"MRFPS_TEST_ADB_EXE":str(cls.adb)}
        script=("$ErrorActionPreference='Stop'; $s=Get-Content -Raw -LiteralPath $env:MRFPS_TEST_ADB_SOURCE; "
                "Add-Type -TypeDefinition $s -OutputAssembly $env:MRFPS_TEST_ADB_EXE -OutputType ConsoleApplication")
        result=subprocess.run([str(POWERSHELL),"-NoProfile","-NonInteractive","-Command",script],
                              env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
        if result.returncode: raise RuntimeError("fixture adb compile failed: "+result.stderr.decode("utf-8","replace"))
        cls.token=cls.root/"fake.jwt"; cls.token.write_text("abc.def.ghi",encoding="ascii")
        cls.public=cls.root/"fake.pem"; cls.public.write_text("-----BEGIN PUBLIC KEY-----\nFAKE\n-----END PUBLIC KEY-----\n",encoding="ascii")
        cls.apk=cls.root/"fixture.apk"; cls.apk.write_bytes(b"fixture")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self,*dumps):
        with tempfile.TemporaryDirectory(prefix="mrfps-fake-adb-run-") as folder:
            temp=Path(folder); log=temp/"calls.txt"
            env={**os.environ,"MRFPS_TEST_ADB_LOG":str(log)}
            for i,dump in enumerate(dumps,1):
                path=temp/("dump"+str(i)+".txt"); path.write_text(dump,encoding="ascii")
                env["MRFPS_TEST_ADB_DUMP"+str(i)]=str(path)
            args=[str(POWERSHELL),"-NoProfile","-NonInteractive","-File",str(ROOT/"Scripts/Deploy-Quest.ps1"),
                  "-SdkRoot",str(self.sdk),"-Serial","fixture","-Server","127.0.0.1","-Apk",str(self.apk),
                  "-TokenFile",str(self.token),"-PublicKeyFile",str(self.public)]
            result=subprocess.run(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
            return result,log.read_text(encoding="utf-8").splitlines()

    def test_pending_or_unreadable_prompt_stops_before_install_and_force_stop(self):
        for dump,message in ((PROMPT,"screen-sharing or privacy permission prompt"),
                             ("unrecognized activity dump","Cannot reliably inspect Quest activity roots")):
            with self.subTest(dump=dump):
                result,calls=self.invoke(dump,dump)
                self.assertNotEqual(result.returncode,0)
                self.assertIn(message, (result.stdout+result.stderr).decode("utf-8",errors="replace"))
                self.assertEqual(calls,["devices -l","-s fixture shell dumpsys activity activities"])
                self.assertNotIn("stack remove", " ".join(calls))

    def test_prompt_appearing_after_install_blocks_force_stop_and_launch(self):
        result,calls=self.invoke(EMPTY,PROMPT)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("screen-sharing or privacy permission prompt", (result.stdout+result.stderr).decode("utf-8",errors="replace"))
        self.assertEqual(calls[0],"devices -l")
        self.assertEqual(calls[1],"-s fixture shell dumpsys activity activities")
        self.assertTrue(calls[2].startswith("-s fixture install -r "))
        self.assertEqual(calls[3],"-s fixture shell dumpsys activity activities")
        self.assertEqual(len(calls),4)

    def test_prompt_appearing_after_wakeup_blocks_activity_start(self):
        result,calls=self.invoke(EMPTY,EMPTY,EMPTY,EMPTY,PROMPT)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("screen-sharing or privacy permission prompt",(result.stdout+result.stderr).decode("utf-8",errors="replace"))
        self.assertIn("-s fixture shell input keyevent KEYCODE_WAKEUP",calls)
        self.assertEqual(calls[-1],"-s fixture shell dumpsys activity activities")
        self.assertFalse(any(" shell am start " in call or " stack remove " in call for call in calls))

    def test_prompt_appearing_during_provisioning_blocks_wakeup(self):
        result,calls=self.invoke(EMPTY,EMPTY,EMPTY,PROMPT)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("screen-sharing or privacy permission prompt",(result.stdout+result.stderr).decode("utf-8",errors="replace"))
        self.assertEqual(calls[-1],"-s fixture shell dumpsys activity activities")
        self.assertFalse(any(" shell input keyevent " in call or " shell am start " in call for call in calls))


if __name__=="__main__":
    unittest.main()
