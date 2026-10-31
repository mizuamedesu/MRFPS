package jp.mrfps.diagnostics;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.os.Looper;
import java.lang.reflect.Method;

/** Shell-only inspection and narrowly scoped cancellation of an orphaned bond. */
public final class QuestBondRecovery {
    private static BluetoothAdapter adapter() throws Exception {
        Looper.prepareMainLooper();
        Class<?> managerClass = Class.forName("android.os.BluetoothServiceManager");
        Object managerInstance = managerClass.getDeclaredConstructor().newInstance();
        Class<?> initializer = Class.forName("android.bluetooth.BluetoothFrameworkInitializer");
        initializer.getDeclaredMethod("setBluetoothServiceManager", managerClass).invoke(null, managerInstance);
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        Object thread = threadClass.getDeclaredMethod("systemMain").invoke(null);
        Context system = (Context) threadClass.getDeclaredMethod("getSystemContext").invoke(thread);
        Context shell = system.createPackageContext("com.android.shell", 0);
        BluetoothManager manager = shell.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null) throw new IllegalStateException("Bluetooth adapter unavailable");
        return adapter;
    }

    public static void main(String[] args) {
        try { execute(args); }
        catch (Exception error) {
            Throwable cause = error.getCause() == null ? error : error.getCause();
            System.out.println("error=" + cause.getClass().getSimpleName() + ": " + cause.getMessage());
        }
    }

    private static void execute(String[] args) throws Exception {
        if (args.length != 2 || !(args[0].equals("inspect") || args[0].equals("cancel-stale") || args[0].equals("remove-orphan")))
            throw new IllegalArgumentException("Expected inspect|cancel-stale|remove-orphan and one address");
        BluetoothDevice device = adapter().getRemoteDevice(args[1]);
        int state = device.getBondState();
        Method connectedMethod = BluetoothDevice.class.getDeclaredMethod("isConnected");
        boolean connected = (Boolean) connectedMethod.invoke(device);
        if (args[0].equals("inspect")) {
            System.out.println("state=" + state + " connected=" + connected);
            return;
        }
        if (state == BluetoothDevice.BOND_NONE) { System.out.println("already_clear"); return; }
        if (state != BluetoothDevice.BOND_BONDING || connected)
            throw new IllegalStateException("Only a disconnected in-progress bond may be cancelled");
        String method = args[0].equals("remove-orphan") ? "removeBond" : "cancelBondProcess";
        Method cancel = BluetoothDevice.class.getDeclaredMethod(method);
        if (!Boolean.TRUE.equals(cancel.invoke(device)))
            throw new IllegalStateException("Android rejected the request; state was not forced");
        for (int i = 0; i < 40; ++i) {
            state = device.getBondState();
            if (state == BluetoothDevice.BOND_NONE) { System.out.println("cleared"); return; }
            if (state == BluetoothDevice.BOND_BONDED)
                throw new IllegalStateException("The device became bonded; it was not removed");
            Thread.sleep(250);
        }
        throw new IllegalStateException("Cancellation was requested but Android has not cleared it");
    }
}
