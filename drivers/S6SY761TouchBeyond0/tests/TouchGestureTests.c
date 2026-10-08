/* SPDX-License-Identifier: GPL-2.0-only */
#include "TouchMock.h"

static void Start(void)
{
    WDFCMRESLIST raw, translated;
    MockReset();
    MockAddDevice();
    MockResources(&raw, &translated);
    CHECK(NT_SUCCESS(MockPrepare(raw, translated)));
    CHECK(NT_SUCCESS(MockStart()));
    CHECK(NT_SUCCESS(MockEnable()));
}

static void Stop(void)
{
    MockStop();
    MockRelease();
}

static void Event(UCHAR* bytes, UCHAR state, UCHAR id, USHORT x, USHORT y)
{
    memset(bytes, 0, 8);
    bytes[0] = (UCHAR)((state << 6) | ((id + 1) << 2));
    bytes[1] = (UCHAR)(x >> 4);
    bytes[2] = (UCHAR)(y >> 4);
    bytes[3] = (UCHAR)((x << 4) | (y & 15));
}

static void Fire(UCHAR state, UCHAR id, USHORT x, USHORT y)
{
    UCHAR bytes[8];
    Event(bytes, state, id, x, y);
    MockFifo(bytes, sizeof(bytes));
    MockFire();
}

static WDFREQUEST Point(UCHAR state, UCHAR id, USHORT x, USHORT y)
{
    WDFREQUEST request = MockSubmitRead();
    const UCHAR* bytes = MO(request)->Buffer;
    CHECK(MO(request)->Completed && MO(request)->Status == STATUS_SUCCESS);
    CHECK(MO(request)->Information == 64 && bytes[0] == 0x54);
    CHECK(bytes[1] == state && bytes[2] == id);
    CHECK(bytes[3] == (x & 255) && bytes[4] == (x >> 8));
    CHECK(bytes[5] == (y & 255) && bytes[6] == (y >> 8));
    return request;
}

static USHORT ScanTime(WDFREQUEST request)
{
    return (USHORT)(MO(request)->Buffer[61] | (MO(request)->Buffer[62] << 8));
}

static void DownOrigin(void)
{
    WDFREQUEST request;
    PDEVICE_CONTEXT d;
    Start();
    d = GetDeviceContext(Mock.Device);
    Mock.InterruptTime = 1000ULL * 1000;
    Fire(1, 0, 100, 200);
    Mock.InterruptTime = 1500ULL * 1000;
    Fire(2, 0, 105, 205);
    Mock.InterruptTime = 2000ULL * 1000;
    Fire(2, 0, 110, 210);
    CHECK(d->InputReportCount == 2 && d->InputReportCoalesceCount == 1);
    request = Point(1, 0, 100, 200);
    CHECK(ScanTime(request) == 1000);
    request = Point(1, 0, 110, 210);
    CHECK(ScanTime(request) == 2000);
    Fire(3, 0, 0, 0);
    Point(0, 0, 110, 210);
    CHECK(!d->Contacts[0].Active && d->InputQueueFlushCount == 0);
    Stop();
}

static void Hold(void)
{
    ULONG i;
    WDFREQUEST request;
    PDEVICE_CONTEXT d;
    Start();
    d = GetDeviceContext(Mock.Device);
    Mock.InterruptTime = 65530ULL * 1000;
    Fire(1, 0, 500, 600);
    for (i = 0; i != 120; ++i) {
        Mock.InterruptTime += 250000; /* 25 ms, including 16-bit scan-time wrap. */
        Fire(2, 0, (USHORT)(500 + (i & 1)), 600);
    }
    CHECK(d->InputReportCount == 2 && d->InputReportCoalesceCount == 119);
    request = Point(1, 0, 500, 600);
    CHECK(ScanTime(request) == 65530);
    request = Point(1, 0, 501, 600);
    CHECK(ScanTime(request) == (USHORT)(65530 + 30000));
    request = MockSubmitRead();
    Mock.InterruptTime += 20000000; /* A stationary, silent hold must stay down. */
    MockFifo(NULL, 0);
    MockFire();
    CHECK(!MO(request)->Completed && d->Contacts[0].Active);
    Fire(3, 0, 0, 0);
    CHECK(MO(request)->Completed && MO(request)->Buffer[1] == 0);
    CHECK(d->InputQueueFlushCount == 0);
    Stop();
}

static void DoubleTap(void)
{
    UCHAR bytes[48];
    Start();
    Event(bytes, 1, 0, 100, 200);
    Event(bytes + 8, 2, 0, 101, 201);
    Event(bytes + 16, 3, 0, 0, 0);
    Event(bytes + 24, 1, 0, 102, 202);
    Event(bytes + 32, 2, 0, 103, 203);
    Event(bytes + 40, 3, 0, 0, 0);
    bytes[7] = 5;
    MockFifo(bytes, sizeof(bytes));
    MockFire();
    CHECK(GetDeviceContext(Mock.Device)->InputReportCount == 6);
    Point(1, 0, 100, 200);
    Point(1, 0, 101, 201);
    Point(0, 0, 101, 201);
    Point(1, 0, 102, 202);
    Point(1, 0, 103, 203);
    Point(0, 0, 103, 203);
    Stop();
}

static void FullFifo(void)
{
    UCHAR bytes[512];
    ULONG i;
    Start();
    for (i = 0; i != 64; ++i) {
        Event(bytes + i * 8, (UCHAR)((i & 1) ? 3 : 1), 0, 100, 200);
    }
    bytes[7] = 63;
    MockFifo(bytes, sizeof(bytes));
    MockFire();
    CHECK(GetDeviceContext(Mock.Device)->InputReportCount == 64);
    CHECK(GetDeviceContext(Mock.Device)->InputQueueFlushCount == 0);
    for (i = 0; i != 64; ++i) { Point((UCHAR)((i & 1) ? 0 : 1), 0, 100, 200); }
    Stop();
}

static void FullQueueMove(void)
{
    ULONG i;
    PDEVICE_CONTEXT d;
    Start();
    d = GetDeviceContext(Mock.Device);
    for (i = 0; i != 31; ++i) {
        Fire(1, 0, 100, 200);
        Fire(3, 0, 0, 0);
    }
    Fire(1, 0, 100, 200);
    Fire(2, 0, 101, 201);
    CHECK(d->InputReportCount == 64);
    Fire(2, 0, 102, 202);
    CHECK(d->InputReportCount == 64 && d->InputQueueFlushCount == 0);
    CHECK(d->InputReportCoalesceCount == 1);
    for (i = 0; i != 31; ++i) { Point(1, 0, 100, 200); Point(0, 0, 100, 200); }
    Point(1, 0, 100, 200);
    Point(1, 0, 102, 202);
    /* Wrap the ring and reuse the same contact ID after a real release. */
    Fire(3, 0, 0, 0);
    Fire(1, 0, 300, 400);
    Fire(2, 0, 301, 401);
    Point(0, 0, 102, 202);
    Point(1, 0, 300, 400);
    Point(1, 0, 301, 401);
    Stop();
}

static void ShortBuffer(void)
{
    ULONG state;
    Start();
    for (state = 1; state <= 3; state += 2) {
        WDFREQUEST bad = MockReadRequest();
        BOOLEAN complete;
        PDEVICE_CONTEXT d = GetDeviceContext(Mock.Device);
        Fire((UCHAR)state, 0, 100, 200);
        MO(bad)->BufferLength = 63;
        CHECK(NT_SUCCESS(ReadReport(GetQueueContext(d->DefaultQueue), bad, &complete)));
        CHECK(!complete && MO(bad)->Completed && !NT_SUCCESS(MO(bad)->Status));
        CHECK(d->InputReportCount == 1);
        Point((UCHAR)(state == 1), 0, 100, 200);
    }
    Stop();
}

static void Multitouch(void)
{
    ULONG id;
    PDEVICE_CONTEXT d;
    WDFREQUEST request;
    Start();
    d = GetDeviceContext(Mock.Device);
    for (id = 0; id != 10; ++id) { Fire(1, (UCHAR)id, (USHORT)(100 + id), 200); }
    Fire(2, 9, 120, 200);
    Fire(2, 9, 121, 200);
    CHECK(d->InputReportCount == 11 && d->InputReportCoalesceCount == 1);
    for (id = 0; id != 10; ++id) {
        request = Point(1, 0, 100, 200);
        CHECK(MO(request)->Buffer[63] == id + 1);
        CHECK(MO(request)->Buffer[1 + id * 6] == 1);
        CHECK(MO(request)->Buffer[2 + id * 6] == id);
    }
    request = Point(1, 0, 100, 200);
    CHECK(MO(request)->Buffer[57] == 121 && MO(request)->Buffer[63] == 10);
    for (id = 0; id != 10; ++id) { Fire(3, (UCHAR)id, 0, 0); }
    for (id = 0; id != 10; ++id) {
        request = MockSubmitRead();
        CHECK(MO(request)->Completed && MO(request)->Buffer[1] == 0);
        CHECK(MO(request)->Buffer[2] == id && MO(request)->Buffer[63] == 10 - id);
    }
    Stop();
}

static void Recovery(void)
{
    ULONG i;
    PDEVICE_CONTEXT d;
    WDFREQUEST request;
    Start();
    d = GetDeviceContext(Mock.Device);
    /* A MOVE without a known down is a new contact, not coalescible onset. */
    Fire(2, 0, 100, 200);
    Fire(2, 0, 101, 201);
    CHECK(d->InputReportCount == 2);
    Point(1, 0, 100, 200);
    Point(1, 0, 101, 201);
    request = MockSubmitRead();
    Mock.FailReadNumber = (int)Mock.Reads + 1;
    Fire(2, 0, 102, 202);
    CHECK(d->InputI2cErrorCount == 1 && !d->Contacts[0].Active);
    CHECK(MO(request)->Completed && MO(request)->Buffer[1] == 0);
    Mock.FailReadNumber = 0;
    for (i = 0; i != 65; ++i) { Fire((UCHAR)((i & 1) ? 3 : 1), 0, 100, 200); }
    CHECK(d->InputQueueFlushCount == 1 && d->InputReportCount == 1);
    Point(0, 0, 100, 200);
    Fire(1, 0, 300, 400);
    Point(1, 0, 300, 400);
    Stop();

    Start();
    d = GetDeviceContext(Mock.Device);
    for (i = 0; i != 31; ++i) {
        Fire(1, 0, 100, 200);
        Fire(3, 0, 0, 0);
    }
    Fire(1, 0, 100, 200);
    Fire(1, 1, 300, 400);
    CHECK(d->InputReportCount == 64);
    Fire(3, 1, 0, 0);
    CHECK(d->InputQueueFlushCount == 1 && d->InputReportCount == 1);
    request = Point(0, 0, 100, 200);
    CHECK(MO(request)->Buffer[63] == 2);
    CHECK(MO(request)->Buffer[7] == 0 && MO(request)->Buffer[8] == 1);
    CHECK(!d->Contacts[0].Active && !d->Contacts[1].Active);
    Stop();
}

static void CancellationAndReentry(void)
{
    WDFREQUEST request;
    Start();
    request = MockSubmitRead();
    MockCancel(request);
    CHECK(MO(request)->Completed && MO(request)->Status == STATUS_CANCELLED);
    request = MockSubmitRead();
    Mock.ReenterOnComplete = TRUE;
    Fire(1, 0, 100, 200);
    CHECK(MO(request)->Completed && Mock.ReenteredRequest != NULL);
    CHECK(!MO(Mock.ReenteredRequest)->Completed);
    Fire(3, 0, 0, 0);
    CHECK(MO(Mock.ReenteredRequest)->Completed && MO(Mock.ReenteredRequest)->Buffer[1] == 0);
    Stop();
}

int main(int argc, char** argv)
{
    static const struct { const char* name; void (*run)(void); } cases[] = {
        {"down-origin", DownOrigin}, {"hold", Hold}, {"double-tap", DoubleTap},
        {"full-fifo", FullFifo}, {"full-queue-move", FullQueueMove},
        {"short-buffer", ShortBuffer}, {"multitouch", Multitouch},
        {"recovery", Recovery}, {"cancellation-reentry", CancellationAndReentry}
    };
    ULONG i;
    CHECK(argc <= 2);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc == 1 || strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            ++TestGroups;
        }
    }
    CHECK(TestGroups != 0);
    printf("{\"groups\":%lu,\"checks\":%lu,\"failures\":0,\"hardwareAccess\":false}\n",
        TestGroups, TestChecks);
    MockReset();
    return 0;
}
