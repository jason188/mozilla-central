/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ 
*/
/* This testcase triggers two telemetry pings.
 * 
 * Telemetry code keeps histograms of past telemetry pings. The first
 * ping populates these histograms. One of those histograms is then
 * checked in the second request.
 */

do_load_httpd_js();
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/LightweightThemeManager.jsm");

const gTestUUID = "3512c938-d9d2-4722-a575-a7f67086d3b2";
const PATH = "/submit/telemetry/test-ping";
const SAVED_PATH = "/submit/telemetry/" + gTestUUID;
const SERVER = "http://localhost:4444";
const IGNORE_HISTOGRAM = "test::ignore_me";
const IGNORE_HISTOGRAM_TO_CLONE = "MEMORY_HEAP_ALLOCATED";
const IGNORE_CLONED_HISTOGRAM = "test::ignore_me_also";
const ADDON_NAME = "Telemetry test addon";
const ADDON_HISTOGRAM = "addon-histogram";

const BinaryInputStream = Components.Constructor(
  "@mozilla.org/binaryinputstream;1",
  "nsIBinaryInputStream",
  "setInputStream");
const Telemetry = Cc["@mozilla.org/base/telemetry;1"].getService(Ci.nsITelemetry);

var httpserver = new nsHttpServer();
var gFinished = false;

function telemetry_ping () {
  const TelemetryPing = Cc["@mozilla.org/base/telemetry-ping;1"].getService(Ci.nsIObserver);
  TelemetryPing.observe(null, "test-gather-startup", null);
  TelemetryPing.observe(null, "test-enable-persistent-telemetry-send", null);
  TelemetryPing.observe(null, "test-ping", SERVER);
}

function nonexistentServerObserver(aSubject, aTopic, aData) {
  Services.obs.removeObserver(nonexistentServerObserver, aTopic);

  httpserver.start(4444);

  // Provide a dummy function so it returns 200 instead of 404 to telemetry.
  httpserver.registerPathHandler(PATH, function () {});
  Services.obs.addObserver(telemetryObserver, "telemetry-test-xhr-complete", false);
  telemetry_ping();
}

function telemetryObserver(aSubject, aTopic, aData) {
  Services.obs.removeObserver(telemetryObserver, aTopic);
  httpserver.registerPathHandler(PATH, checkPersistedHistograms);
  Telemetry.newHistogram(IGNORE_HISTOGRAM, 1, 2, 3, Telemetry.HISTOGRAM_BOOLEAN);
  Telemetry.histogramFrom(IGNORE_CLONED_HISTOGRAM, IGNORE_HISTOGRAM_TO_CLONE);
  Services.startup.interrupted = true;
  let dirService = Cc["@mozilla.org/file/directory_service;1"]
                    .getService(Ci.nsIProperties);
  let tmpDir = dirService.get("TmpD", Ci.nsILocalFile);
  let histogramsFile = tmpDir.clone();
  histogramsFile.append("saved-histograms.dat");
  if (histogramsFile.exists()) {
    histogramsFile.remove(true);
  }
  do_register_cleanup(function () histogramsFile.remove(true));
  const TelemetryPing = Cc["@mozilla.org/base/telemetry-ping;1"].getService(Ci.nsIObserver);
  TelemetryPing.observe(histogramsFile, "test-save-histograms", gTestUUID);
  TelemetryPing.observe(histogramsFile, "test-load-histograms", null);

  Telemetry.registerAddonHistogram(ADDON_NAME, ADDON_HISTOGRAM, 1, 5, 6,
                                   Telemetry.HISTOGRAM_LINEAR);
  h1 = Telemetry.getAddonHistogram(ADDON_NAME, ADDON_HISTOGRAM);
  h1.add(1);

  telemetry_ping();
}

function decodeRequestPayload(request) {
  let s = request.bodyInputStream;
  let payload = null;
  let decoder = Cc["@mozilla.org/dom/json;1"].createInstance(Ci.nsIJSON)

  if (request.getHeader("content-encoding") == "gzip") {
    let observer = {
      buffer: "",
      onStreamComplete: function(loader, context, status, length, result) {
	this.buffer = String.fromCharCode.apply(this, result);
      }
    };

    let scs = Cc["@mozilla.org/streamConverters;1"]
              .getService(Ci.nsIStreamConverterService);
    let listener = Cc["@mozilla.org/network/stream-loader;1"]
                  .createInstance(Ci.nsIStreamLoader);
    listener.init(observer);
    let converter = scs.asyncConvertData("gzip", "uncompressed",
                                         listener, null);
    converter.onStartRequest(null, null);
    converter.onDataAvailable(null, null, s, 0, s.available());
    converter.onStopRequest(null, null, null);
    payload = decoder.decode(observer.buffer);
  } else {
    payload = decoder.decodeFromStream(s, s.available());
  }

  return payload;
}

function checkPayloadInfo(payload, reason) {
  // get rid of the non-deterministic field
  const expected_info = {
    OS: "XPCShell", 
    appID: "xpcshell@tests.mozilla.org", 
    appVersion: "1", 
    appName: "XPCShell", 
    appBuildID: "2007010101",
    platformBuildID: "2007010101"
  };

  for (let f in expected_info) {
    do_check_eq(payload.info[f], expected_info[f]);
  }

  do_check_eq(payload.info.reason, reason);
  do_check_true("appUpdateChannel" in payload.info);
  do_check_true("locale" in payload.info);

  try {
    // If we've not got nsIGfxInfoDebug, then this will throw and stop us doing
    // this test.
    var gfxInfo = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfoDebug);
    var isWindows = ("@mozilla.org/windows-registry-key;1" in Components.classes);
    var isOSX = ("nsILocalFileMac" in Components.interfaces);

    if (isWindows || isOSX) {
      do_check_true("adapterVendorID" in payload.info);
      do_check_true("adapterDeviceID" in payload.info);
    }
  }
  catch (x) {
  }
}

function checkPersistedHistograms(request, response) {
  let payload = decodeRequestPayload(request);

  checkPayloadInfo(payload, "saved-session");
  httpserver.registerPathHandler(PATH, checkHistograms);
}

function checkHistograms(request, response) {
  // do not need the http server anymore
  httpserver.stop(do_test_finished);
  let payload = decodeRequestPayload(request);

  checkPayloadInfo(payload, "test-ping");
  do_check_eq(request.getHeader("content-type"), "application/json; charset=UTF-8");
  do_check_true(payload.simpleMeasurements.uptime >= 0);
  do_check_true(payload.simpleMeasurements.startupInterrupted === 1);

  const TELEMETRY_PING = "TELEMETRY_PING";
  const TELEMETRY_SUCCESS = "TELEMETRY_SUCCESS";
  const TELEMETRY_TEST_FLAG = "TELEMETRY_TEST_FLAG";
  do_check_true(TELEMETRY_PING in payload.histograms);
  let rh = Telemetry.registeredHistograms;
  for (let name in rh) {
    if (/SQLITE/.test(name) && name in payload.histograms) {
      do_check_true(("STARTUP_" + name) in payload.histograms); 
    }
  }
  do_check_false(IGNORE_HISTOGRAM in payload.histograms);
  do_check_false(IGNORE_CLONED_HISTOGRAM in payload.histograms);

  // Flag histograms should automagically spring to life.
  const expected_flag = {
    range: [1, 2],
    bucket_count: 3,
    histogram_type: 3,
    values: {0:1, 1:0},
    sum: 1
  };
  let flag = payload.histograms[TELEMETRY_TEST_FLAG];
  do_check_eq(uneval(flag), uneval(expected_flag));

  // There should be one successful report from the previous telemetry ping.
  const expected_tc = {
    range: [1, 2],
    bucket_count: 3,
    histogram_type: 2,
    values: {0:1, 1:1, 2:0},
    sum: 1
  };
  let tc = payload.histograms[TELEMETRY_SUCCESS];
  do_check_eq(uneval(tc), 
              uneval(expected_tc));

  // The ping should include data from memory reporters.  We can't check that
  // this data is correct, because we can't control the values returned by the
  // memory reporters.  But we can at least check that the data is there.
  //
  // It's important to check for the presence of reporters with a mix of units,
  // because TelemetryPing has separate logic for each one.  But we can't
  // currently check UNITS_COUNT_CUMULATIVE or UNITS_PERCENTAGE because
  // Telemetry doesn't touch a memory reporter with these units that's
  // available on all platforms.

  do_check_true('MEMORY_JS_GC_HEAP' in payload.histograms); // UNITS_BYTES
  do_check_true('MEMORY_JS_COMPARTMENTS_SYSTEM' in payload.histograms); // UNITS_COUNT

  // We should have included addon histograms.
  do_check_true("addonHistograms" in payload);
  do_check_true(ADDON_NAME in payload.addonHistograms);
  do_check_true(ADDON_HISTOGRAM in payload.addonHistograms[ADDON_NAME]);

  do_check_true(("mainThread" in payload.slowSQL) &&
                ("otherThreads" in payload.slowSQL));
  gFinished = true;
}

// copied from toolkit/mozapps/extensions/test/xpcshell/head_addons.js
const XULAPPINFO_CONTRACTID = "@mozilla.org/xre/app-info;1";
const XULAPPINFO_CID = Components.ID("{c763b610-9d49-455a-bbd2-ede71682a1ac}");

function createAppInfo(id, name, version, platformVersion) {
  gAppInfo = {
    // nsIXULAppInfo
    vendor: "Mozilla",
    name: name,
    ID: id,
    version: version,
    appBuildID: "2007010101",
    platformVersion: platformVersion,
    platformBuildID: "2007010101",

    // nsIXULRuntime
    inSafeMode: false,
    logConsoleErrors: true,
    OS: "XPCShell",
    XPCOMABI: "noarch-spidermonkey",
    invalidateCachesOnRestart: function invalidateCachesOnRestart() {
      // Do nothing
    },

    // nsICrashReporter
    annotations: {},

    annotateCrashReport: function(key, data) {
      this.annotations[key] = data;
    },

    QueryInterface: XPCOMUtils.generateQI([Ci.nsIXULAppInfo,
                                           Ci.nsIXULRuntime,
                                           Ci.nsICrashReporter,
                                           Ci.nsISupports])
  };

  var XULAppInfoFactory = {
    createInstance: function (outer, iid) {
      if (outer != null)
        throw Components.results.NS_ERROR_NO_AGGREGATION;
      return gAppInfo.QueryInterface(iid);
    }
  };
  var registrar = Components.manager.QueryInterface(Ci.nsIComponentRegistrar);
  registrar.registerFactory(XULAPPINFO_CID, "XULAppInfo",
                            XULAPPINFO_CONTRACTID, XULAppInfoFactory);
}

function dummyTheme(id) {
  return {
    id: id,
    name: Math.random().toString(),
    headerURL: "http://lwttest.invalid/a.png",
    footerURL: "http://lwttest.invalid/b.png",
    textcolor: Math.random().toString(),
    accentcolor: Math.random().toString()
  };
}

function run_test() {
  try {
    var gfxInfo = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfoDebug);
    gfxInfo.spoofVendorID("0xabcd");
    gfxInfo.spoofDeviceID("0x1234");
  } catch (x) {
    // If we can't test gfxInfo, that's fine, we'll note it later.
  }

  // Addon manager needs a profile directory
  do_get_profile();
  createAppInfo("xpcshell@tests.mozilla.org", "XPCShell", "1", "1.9.2");
  // try to make LightweightThemeManager do stuff
  let gInternalManager = Cc["@mozilla.org/addons/integration;1"]
                         .getService(Ci.nsIObserver)
                         .QueryInterface(Ci.nsITimerCallback);

  gInternalManager.observe(null, "addons-startup", null);
  LightweightThemeManager.currentTheme = dummyTheme("1234");

  Services.obs.addObserver(nonexistentServerObserver, "telemetry-test-xhr-complete", false);
  telemetry_ping();
  // spin the event loop
  do_test_pending();
  // ensure that test runs to completion
  do_register_cleanup(function () do_check_true(gFinished));
 }
