//
//  G814Keys.cpp
//
//  The ASUS ROG Strix G814JI's Fn hotkeys under macOS.
//
//  The internal keyboard (USB 0b05:19b6, ASUS "N-KEY") reports its Fn hotkeys in input report 0x5a on the vendor usage
//  page 0xff31, one usage per key (0x10 brightness down, 0x20 brightness up, 0xc4 / 0xc5 keyboard light up / down), which
//  macOS's HID event driver does not read. On Windows, ASUS's software passes the usage to the firmware through the ATK
//  WMI method (WMNB, DEVS 0x00100021 -> \_SB.PC00.LPCB.EC0.SKEY); for the brightness keys SKEY runs _Q17 / _Q18 -> QLIB ->
//  BRTN, which sends the ACPI video notification Notify (\_SB.PC00.GFX0.DD1F, 0x87 / 0x86). G814Keys reads report 0x5a
//  on its way through AppleUserHIDDevice::handleReportWithTime and calls SKEY with the usage the same way, off the report
//  path; BrightnessKeys turns the notification into the brightness keys. The keyboard light has no firmware path on this
//  model (its WMI device 0x00050021 answers unsupported), so its keys set the keyboard's level with the feature report
//  Linux's hid-asus sends.
//
//  The report format, the handshake and the keyboard-light command are those of Linux's drivers/hid/hid-asus.c.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <kern/thread_call.h>

static const char *bootargOff[] {"-g814keysoff"};
static const char *bootargDebug[] {"-g814keysdbg"};

static void pluginStart();

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	nullptr,
	0,
	KernelVersion::Sequoia,
	KernelVersion::Sequoia,
	pluginStart,
};

namespace {

constexpr uint32_t AsusVendorID = 0x0b05;
constexpr uint32_t NKeyProductID = 0x19b6;

// hid-asus.c: FEATURE_KBD_REPORT_ID (the hotkeys' input report, and the keyboard's command report), FEATURE_KBD_LED_REPORT_ID1,
// FEATURE_KBD_REPORT_SIZE
constexpr uint8_t KbdReportID = 0x5a;
constexpr uint8_t KbdLedReportID = 0x5d;
constexpr size_t KbdReportSize = 64;

constexpr uint8_t KeyBrightnessDown = 0x10;
constexpr uint8_t KeyBrightnessUp = 0x20;
constexpr uint8_t KeyKbdLightUp = 0xc4;
constexpr uint8_t KeyKbdLightDown = 0xc5;

// The firmware's hotkey entry (ATKD WMNB DEVS 0x00100021), and the display id BRTN tests before it notifies DD1F
const char SkeyPath[] = "\\_SB.PC00.LPCB.EC0.SKEY";
const char DidxPath[] = "\\DIDX";

constexpr uint32_t MaxRows = 48;

const char *pathIOHIDFamily[] {"/System/Library/Extensions/IOHIDFamily.kext/Contents/MacOS/IOHIDFamily"};
KernelPatcher::KextInfo kextIOHIDFamily {"com.apple.iokit.IOHIDFamily", pathIOHIDFamily, arrsize(pathIOHIDFamily), {true}, {},
	KernelPatcher::KextInfo::Unloaded};

mach_vm_address_t orgHandleReportWithTime;

// The keyboard's HID device, retained while it is published; the report path only compares against it.
IOHIDDevice *volatile nkey;
uint8_t lastKey;

// Keys pressed on the report path, handled by the worker thread call (SKEY and the feature reports may block).
IOSimpleLock *queueLock;
uint8_t queue[16];
uint32_t queueHead, queueTail;
thread_call_t worker;
IOLock *workerLock;
bool needInit;
bool kbdLightSupported;
int kbdLevel = -1;

volatile uint32_t rows;
#define ROW(...) do { if (__sync_fetch_and_add(&rows, 1) < MaxRows) SYSLOG("g814keys", __VA_ARGS__); } while (0)

IOACPIPlatformDevice *acpiDevice() {
	static IOACPIPlatformDevice *device;
	if (!device) {
		auto entry = IORegistryEntry::fromPath("IOACPIPlane:/_SB/PC00@0");
		device = OSDynamicCast(IOACPIPlatformDevice, entry);
		if (!device)
			OSSafeReleaseNULL(entry);
	}
	return device;
}

IOReturn setFeature(IOHIDDevice *device, const uint8_t *bytes, size_t size) {
	auto report = IOBufferMemoryDescriptor::withBytes(bytes, size, kIODirectionOut);
	if (!report)
		return kIOReturnNoMemory;
	auto status = device->setReport(report, kIOHIDReportTypeFeature, bytes[0]);
	report->release();
	return status;
}

IOReturn getFeature(IOHIDDevice *device, uint8_t id, uint8_t *bytes, size_t size) {
	auto report = IOBufferMemoryDescriptor::withCapacity(size, kIODirectionIn);
	if (!report)
		return kIOReturnNoMemory;
	report->setLength(size);
	auto status = device->getReport(report, kIOHIDReportTypeFeature, id);
	if (status == kIOReturnSuccess)
		report->readBytes(0, bytes, size);
	report->release();
	return status;
}

// hid-asus.c asus_initialize_reports / asus_kbd_init: the handshake, set and read back, on each of the keyboard's report IDs;
// asus_kbd_get_functions: byte 6 of the reply, bit 0 = the keyboard light.
void initKeyboard(IOHIDDevice *device) {
	static const uint8_t ids[] {KbdReportID, KbdLedReportID};
	for (auto id : ids) {
		const uint8_t handshake[] {id, 'A', 'S', 'U', 'S', ' ', 'T', 'e', 'c', 'h', '.', 'I', 'n', 'c', '.', 0};
		uint8_t ack[KbdReportSize] {};
		auto set = setFeature(device, handshake, sizeof(handshake));
		auto get = getFeature(device, id, ack, sizeof(ack));
		ROW("handshake 0x%02x: set 0x%x, get 0x%x, %s", id, set, get, memcmp(ack, handshake, sizeof(handshake)) ? "no echo" : "echoed");
	}

	const uint8_t query[] {KbdReportID, 0x05, 0x20, 0x31, 0x00, 0x08};
	uint8_t reply[KbdReportSize] {};
	auto set = setFeature(device, query, sizeof(query));
	auto get = getFeature(device, KbdReportID, reply, sizeof(reply));
	kbdLightSupported = set == kIOReturnSuccess && get == kIOReturnSuccess && (reply[6] & 1);
	ROW("functions 0x%02x (set 0x%x, get 0x%x): keyboard light %s", reply[6], set, get, kbdLightSupported ? "yes" : "no");

	uint32_t didx = 0;
	auto acpi = acpiDevice();
	auto status = acpi ? acpi->evaluateInteger(DidxPath, &didx) : kIOReturnNotFound;
	ROW("DIDX 0x%x (0x%x): BRTN %s DD1F", didx, status, (didx & 0x0f0f) == 0x0400 ? "notifies" : "does not notify");
}

IOReturn callSkey(uint8_t key, uint32_t *ret) {
	auto acpi = acpiDevice();
	if (!acpi)
		return kIOReturnNotFound;
	auto arg = OSNumber::withNumber(key, 32);
	if (!arg)
		return kIOReturnNoMemory;
	OSObject *params[] {arg};
	OSObject *result = nullptr;
	auto status = acpi->evaluateObject(SkeyPath, &result, params, 1);
	if (auto number = OSDynamicCast(OSNumber, result))
		*ret = number->unsigned32BitValue();
	OSSafeReleaseNULL(result);
	arg->release();
	return status;
}

// hid-asus.c asus_kbd_backlight_set: levels 0-3. The keyboard's level cannot be read, so the first key starts from 2.
void setKbdLight(IOHIDDevice *device, uint8_t key) {
	if (!kbdLightSupported) {
		ROW("key 0x%02x: no keyboard light", key);
		return;
	}
	if (kbdLevel < 0)
		kbdLevel = 2;
	kbdLevel += key == KeyKbdLightUp ? 1 : -1;
	kbdLevel = kbdLevel < 0 ? 0 : kbdLevel > 3 ? 3 : kbdLevel;
	const uint8_t command[KbdReportSize] {KbdReportID, 0xba, 0xc5, 0xc4, static_cast<uint8_t>(kbdLevel)};
	auto status = setFeature(device, command, sizeof(command));
	ROW("key 0x%02x: keyboard light %d (0x%x)", key, kbdLevel, status);
}

void handleKey(IOHIDDevice *device, uint8_t key) {
	switch (key) {
		case KeyBrightnessDown:
		case KeyBrightnessUp: {
			uint32_t ret = 0;
			auto status = callSkey(key, &ret);
			ROW("key 0x%02x: SKEY 0x%x, returned %u", key, status, ret);
			break;
		}
		case KeyKbdLightUp:
		case KeyKbdLightDown:
			setKbdLight(device, key);
			break;
		default:
			ROW("key 0x%02x: not handled", key);
			break;
	}
}

void runWorker(thread_call_param_t, thread_call_param_t) {
	IOLockLock(workerLock);
	auto device = nkey;
	if (device && needInit) {
		needInit = false;
		initKeyboard(device);
	}
	while (true) {
		uint8_t key = 0;
		IOSimpleLockLock(queueLock);
		bool pending = queueHead != queueTail;
		if (pending)
			key = queue[queueHead++ % arrsize(queue)];
		IOSimpleLockUnlock(queueLock);
		if (!pending)
			break;
		if (device)
			handleKey(device, key);
	}
	IOLockUnlock(workerLock);
}

// Only the first of the report's five usage slots is read, as hid-asus fixes the descriptor's count to 1: the others carry
// companion bytes (the camera key's state byte 0x10 would read as brightness down). A key acts once, when its usage appears.
void noteReport(IOMemoryDescriptor *report) {
	uint8_t data[2] {};
	if (report->getLength() < sizeof(data) || report->readBytes(0, data, sizeof(data)) != sizeof(data) || data[0] != KbdReportID)
		return;
	auto key = data[1];
	if (key == lastKey)
		return;
	lastKey = key;
	if (key == 0)
		return;
	IOSimpleLockLock(queueLock);
	if (queueTail - queueHead < arrsize(queue))
		queue[queueTail++ % arrsize(queue)] = key;
	IOSimpleLockUnlock(queueLock);
	thread_call_enter(worker);
}

IOReturn wrapHandleReportWithTime(IOHIDDevice *that, uint64_t timeStamp, IOMemoryDescriptor *report, IOHIDReportType reportType,
	IOOptionBits options) {
	if (that == nkey && reportType == kIOHIDReportTypeInput && report)
		noteReport(report);
	return FunctionCast(wrapHandleReportWithTime, orgHandleReportWithTime)(that, timeStamp, report, reportType, options);
}

bool hasHotkeyReport(IOService *service) {
	auto descriptor = OSDynamicCast(OSData, service->getProperty(kIOHIDReportDescriptorKey));
	if (!descriptor)
		return false;
	auto bytes = static_cast<const uint8_t *>(descriptor->getBytesNoCopy());
	for (unsigned i = 0; i + 1 < descriptor->getLength(); i++)
		if (bytes[i] == 0x85 && bytes[i + 1] == KbdReportID)
			return true;
	return false;
}

bool nkeyPublished(void *, void *, IOService *service, IONotifier *) {
	auto device = OSDynamicCast(IOHIDDevice, service);
	if (!device || !hasHotkeyReport(device))
		return true;
	IOLockLock(workerLock);
	bool taken = nkey == nullptr;
	if (taken) {
		device->retain();
		lastKey = 0;
		needInit = true;
		nkey = device;
	}
	IOLockUnlock(workerLock);
	ROW("N-KEY %04x:%04x registry id 0x%llx %s", AsusVendorID, NKeyProductID, device->getRegistryEntryID(), taken ? "taken" : "ignored, one is taken");
	if (taken) {
		uint64_t deadline;
		clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
		thread_call_enter_delayed(worker, deadline);
	}
	return true;
}

bool nkeyTerminated(void *, void *, IOService *service, IONotifier *) {
	IOLockLock(workerLock);
	IOHIDDevice *device = nullptr;
	if (service == nkey) {
		device = nkey;
		nkey = nullptr;
	}
	IOLockUnlock(workerLock);
	if (device) {
		ROW("N-KEY registry id 0x%llx terminated", device->getRegistryEntryID());
		device->release();
	}
	return true;
}

void watchKeyboard() {
	auto matching = IOService::serviceMatching("IOHIDDevice");
	auto properties = OSDictionary::withCapacity(2);
	auto vendor = OSNumber::withNumber(AsusVendorID, 32);
	auto product = OSNumber::withNumber(NKeyProductID, 32);
	bool ok = matching && properties && vendor && product && properties->setObject(kIOHIDVendorIDKey, vendor) &&
		properties->setObject(kIOHIDProductIDKey, product) && matching->setObject(gIOPropertyMatchKey, properties);
	if (ok)
		ok = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, nkeyPublished, nullptr) &&
			IOService::addMatchingNotification(gIOTerminatedNotification, matching, nkeyTerminated, nullptr);
	OSSafeReleaseNULL(vendor);
	OSSafeReleaseNULL(product);
	OSSafeReleaseNULL(properties);
	OSSafeReleaseNULL(matching);
	if (!ok)
		SYSLOG("g814keys", "cannot watch for the keyboard");
}

void processKext(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index != kextIOHIDFamily.loadIndex)
		return;
	KernelPatcher::RouteRequest request {"__ZN18AppleUserHIDDevice20handleReportWithTimeEyP18IOMemoryDescriptor15IOHIDReportTypej",
		wrapHandleReportWithTime, orgHandleReportWithTime};
	bool routed = patcher.routeMultiple(index, &request, 1, address, size);
	SYSLOG("g814keys", "AppleUserHIDDevice::handleReportWithTime %s", routed ? "routed" : "not routed: the keys stay unhandled");
	if (!routed)
		patcher.clearError();
}

}	// namespace

static void pluginStart() {
	queueLock = IOSimpleLockAlloc();
	workerLock = IOLockAlloc();
	worker = thread_call_allocate(runWorker, nullptr);
	if (!queueLock || !workerLock || !worker) {
		SYSLOG("g814keys", "allocation failed");
		return;
	}
	lilu.onKextLoadForce(&kextIOHIDFamily, 1, processKext);
	watchKeyboard();
}
