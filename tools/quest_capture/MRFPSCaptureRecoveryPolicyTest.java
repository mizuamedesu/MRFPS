package com.mrfps.capture;

/** No Android runtime or headset is needed. Exercises the production policy. */
public final class MRFPSCaptureRecoveryPolicyTest {
    private static int assertions;

    private static void expect(boolean ok, String message) {
        ++assertions;
        if (!ok) throw new AssertionError(message);
    }

    private static void expectDecision(MRFPSCaptureRecoveryPolicy policy, long now,
                                       boolean active, boolean eligible,
                                       MRFPSCaptureRecoveryPolicy.Decision expected) {
        expect(policy.decide(now, active, eligible) == expected,
                "unexpected decision at " + now + ", expected " + expected);
    }

    private static void badThrough(MRFPSCaptureRecoveryPolicy policy, long start, long end) {
        for (long now = start; now <= end; now += 1_000) policy.badOutput(now);
    }

    private static void watchdogStages() {
        for (String stage : new String[] {"acquire", "pixels", "jpeg", "jni", "projection_start", "surface_rebind"}) {
            expect(MRFPSCaptureRecoveryPolicy.watchdogBusy(stage), "unwatched stage: " + stage);
        }
        for (String stage : new String[] {null, "created", "idle", "release"}) {
            expect(!MRFPSCaptureRecoveryPolicy.watchdogBusy(stage), "unexpected busy stage: " + stage);
        }
    }

    private static void persistentBadOutputIsBounded() {
        MRFPSCaptureRecoveryPolicy policy = new MRFPSCaptureRecoveryPolicy();
        badThrough(policy, 0, 2_000);
        expectDecision(policy, 2_999, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        policy.badOutput(3_000);
        expectDecision(policy, 3_000, false, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        expectDecision(policy, 3_000, true, false, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        expectDecision(policy, 3_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        badThrough(policy, 4_000, 6_000);
        expectDecision(policy, 6_000, true, false, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        expectDecision(policy, 6_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        badThrough(policy, 7_000, 12_000);
        expectDecision(policy, 12_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        expect(policy.badOutputRebinds() == 3, "exactly three recovery attempts");
        badThrough(policy, 13_000, 24_000);
        expectDecision(policy, 24_000, false, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        expectDecision(policy, 24_000, true, false, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        expectDecision(policy, 24_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.STOP);
        expect(policy.badOutputRebinds() == 3, "stop must not begin a fourth rebind");
    }

    private static void onlyAcceptedOutputResetsAttempts() {
        MRFPSCaptureRecoveryPolicy policy = new MRFPSCaptureRecoveryPolicy();
        badThrough(policy, 0, 3_000);
        expectDecision(policy, 3_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        // Image arrivals, an oversized encode, or a returned-but-rejected JNI
        // call report badOutput, not acceptedOutput: none is a recovery.
        badThrough(policy, 4_000, 6_000);
        expectDecision(policy, 6_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        expect(policy.badOutputRebinds() == 2, "bad producer activity must not reset attempts");
        policy.acceptedOutput();
        expect(policy.badOutputRebinds() == 0, "accepted native frame resets the incident");
        expectDecision(policy, 6_001, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        badThrough(policy, 7_000, 10_000);
        expectDecision(policy, 10_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
    }

    private static void silenceAndClockDiscontinuityAreNotBadOutput() {
        MRFPSCaptureRecoveryPolicy policy = new MRFPSCaptureRecoveryPolicy();
        policy.badOutput(-1);
        expectDecision(policy, 5_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        policy.badOutput(1_000);
        expectDecision(policy, 9_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        policy.badOutput(9_000); // long silence starts a new continuous interval
        expectDecision(policy, 9_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        policy.badOutput(500); // clock rollback also starts a new interval
        expectDecision(policy, 500, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        badThrough(policy, 1_500, 3_500);
        expectDecision(policy, 3_500, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
    }

    private static void slowBadFramesStillRecover() {
        MRFPSCaptureRecoveryPolicy policy = new MRFPSCaptureRecoveryPolicy();
        policy.badOutput(0);
        expectDecision(policy, 3_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        policy.badOutput(4_000);
        expectDecision(policy, 4_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
    }

    private static void visibilityPausesJudgementWithoutFakingRecovery() {
        MRFPSCaptureRecoveryPolicy policy = new MRFPSCaptureRecoveryPolicy();
        badThrough(policy, 0, 3_000);
        expectDecision(policy, 3_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        badThrough(policy, 4_000, 6_000);
        expectDecision(policy, 6_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        badThrough(policy, 7_000, 12_000);
        expectDecision(policy, 12_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.REBIND);
        badThrough(policy, 13_000, 24_000);
        policy.suspend();
        expect(policy.badOutputRebinds() == 3, "hiding output must not claim recovery");
        policy.badOutput(24_000);
        expectDecision(policy, 24_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.WAIT);
        badThrough(policy, 25_000, 27_000);
        expectDecision(policy, 27_000, true, true, MRFPSCaptureRecoveryPolicy.Decision.STOP);
    }

    public static void main(String[] args) {
        watchdogStages();
        persistentBadOutputIsBounded();
        onlyAcceptedOutputResetsAttempts();
        silenceAndClockDiscontinuityAreNotBadOutput();
        slowBadFramesStillRecover();
        visibilityPausesJudgementWithoutFakingRecovery();
        System.out.println("PASS MRFPSCaptureRecoveryPolicy assertions=" + assertions);
    }
}
