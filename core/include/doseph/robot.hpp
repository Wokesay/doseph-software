// SPDX-License-Identifier: AGPL-3.0-or-later
// Composition root of the core: connects hardware, sensor truth, gateway,
// doser, dosing journal, cycle, watchdog, history and storage. Runs as one
// event loop with an injected clock (rule R8); the API calls the public
// methods under the robot's mutex.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "doseph/alarms.hpp"
#include "doseph/auth.hpp"
#include "doseph/coupling.hpp"
#include "doseph/mqtt.hpp"
#include "doseph/push.hpp"
#include "doseph/refill.hpp"
#include "doseph/climate.hpp"
#include "doseph/lighting.hpp"
#include "doseph/watering.hpp"
#include "doseph/service.hpp"
#include "doseph/clock.hpp"
#include "doseph/cycle.hpp"
#include "doseph/history.hpp"
#include "doseph/pumpcal.hpp"
#include "doseph/watchdog.hpp"

namespace doseph {

struct Result {
  int status = 200;
  json body = json::object();
  static Result ok(json b = json::object()) { return {200, std::move(b)}; }
  // An error is a message with key and values, shown in the page's language.
  static Result fail(int st, const Msg& m, json extra = json::object());
};

// A channel's calibration or priming run, into a cup or the line (RAT-070).
// A run of one channel by hand; a calibration is a pump calibration of its own (SD-143)
struct ChannelJob {
  std::string id, type;  // prime (into a cup) | fill (into the tank, SD-111)
  int channel = 0;
  std::string head;
  std::string state = "running";  // running | done | failed
  Ms ms = 0, actualMs = 0;
  Msg message;
  Epoch at = 0;
};

class Robot : public RemoteRuns {
 public:
  Robot(IHardware& hw, IStorage& storage, const IClock& clock, RandomFn rng);

  void boot();
  void tick();
  void flush();     // saves what is not on disk yet: state, new events, history
  bool saveAuth();  // the password file, on disk when it returns true

  std::recursive_mutex& mutex() { return mtx_; }
  const Config& config() const { return cfg_; }
  const RuntimeState& runtime() const { return rt_; }
  Auth& auth() { return auth_; }
  // The password was set but auth.json is gone: no setup over the network.
  bool credentialsLost() const { return cfg_.system.passwordSet && !auth_.hasPassword(); }
  // auth.json is there but holds no password the robot can read: it is kept
  // as it is, no new password goes over it, and nobody is told to reset the
  // robot, since a restart may read it again.
  bool passwordKept() const { return authKept_; }
  void markPasswordSet();
  // The platform shows the code to its owner alone (a label, a display, the
  // simulator's console); "" when a password is set.
  std::string armSetupCode(Ms validFor = 0);
  void setPlatform(json p) { platform_ = std::move(p); }
  // The device's own identity (SD-129): random, made at the first start, kept apart from the
  // configuration so a restored backup does not copy it
  const std::string& deviceId() const { return deviceId_; }
  // The platform shows setup codes to whoever stands at the device (a label, a display, the
  // simulator's console); set before boot. The robot then arms one whenever a first password is due
  // (the first start, an access reset), under the same lock, so the first password always needs it
  // (SD-129, SD-139).
  void showSetupCodes(bool on) { showCodes_ = on; }
  // The code armed since the last call, for the platform to show; "" for none
  std::string takeCodeToShow();

  // ---- Resets (SD-139)
  // An access reset, asked for at the device: no password, no session, no access token, no
  // coupling, and MQTT takes no commands. Dosing and every other setting stay.
  void resetAccess();
  // A factory reset, asked for at the device or by the owner with the password: everything off,
  // every file removed; then the platform restarts the device, which starts as new
  void factoryReset(const std::string& by);
  Result factoryResetByOwner(const json& j, const std::string& client);
  // A factory reset is done: the platform restarts the device; until then nothing is saved
  bool restartWanted() const { return restartWanted_; }
  // A factory reset is under way or done: the API takes nothing more
  bool halted() const;
  // The function key's window (SD-149): a short press opens it for 5 min. Within it the code on the
  // device's label sets the first password and pairs this device as the follower; five wrong codes
  // close it. Memory only: a restart closes it.
  static constexpr Ms kKeyWindowMs = 5 * kMinute;
  static constexpr int kKeyWrongMax = 5;
  bool hasLabelCode() const { return hw_.labelCode().has_value(); }
  bool keyWindowOpen() const;
  void wrongLabelCode();
  void endKeyWindow();  // the simulator's demo, once it set its password

  // ---- Coupling (PD-457, SD-131): the peer, pairing with a code, leaving
  json coupling() const;
  // On the device to be coupled as the follower: a code to show, 10 minutes valid
  Result armPairingCode();
  // On the leader: pairs with the device at `address` that shows `code`; the answer comes later
  Result pair(const json& j);
  // On the follower, called by the leader without sign-in: the code is the credential
  Result join(const json& j);
  // Ends the coupling here and tells the other device where this one can reach it
  Result unpair();
  // The other device ended the coupling; `token` is the credential it called with
  Result left(const std::string& token);
  // Whether a credential is the token this device made for its peer
  bool isPeerToken(const std::string& token) const;
  // On the follower, called by the leader every 2 s (SD-131): whether its feed pump runs; answers
  // with what this device measures, whether its pump moves water, and its run
  Result peerBeat(const std::string& token, const json& j);
  // On the follower: a run on one of its channels, on the leader's request; refused without the
  // link, while another runs, or by its own gateway
  Result peerRun(const std::string& token, const json& j);
  Result peerAbort(const std::string& token);
  // On the leader (SD-131): the runs on the follower's channels, for the doser
  bool linked() const override;
  void send(const std::string& job, int channel, Ms ms, const std::string& purpose) override;
  std::optional<RemoteResult> result(const std::string& job) override;
  void abort(const std::string& job) override;
  // What the platform sends to the other device, and the answers it gets
  std::vector<PeerRequest> takePeerRequests();
  void peerResponse(long long id, int status, const json& body);

  // ---- Network devices (SD-132): Shelly Gen2+, ESPHome, MQTT; roles bound to them
  json devices() const;
  Result addDevice(const json& j);
  Result removeDevice(const std::string& id);
  // A device's credentials {user, password}; kept apart from the configuration, never shown
  Result putDeviceCredentials(const std::string& id, const json& j);
  Result putRole(const std::string& role, const json& j);
  Result deleteRole(const std::string& role);
  // What the platform sends to the devices, and the answers it gets (status 0: not reached)
  std::vector<NetRequest> takeDeviceRequests();
  void deviceResponse(long long id, int status, const json& body);
  // Logic says what it wants of an output role; the gateway decides (R1). False for a role that is
  // not an output.
  bool wantOutput(const std::string& role, bool on);
  // The refill valve (SD-132): a person's "Fill now" and stop, and its longest fill {maxFillMin}
  Result startRefill();
  Result stopRefill();
  Result putRefill(const json& j);
  // The circulation pump (SD-132): {mode: "mixing" | "always"}
  Result putCirculation(const json& j);
  // Watering (SD-134): the reserve {reservePct}; a circuit (a new one without an ID) and its
  // removal; "water now" {seconds, override}, its stop, and pausing a circuit's schedule {paused}
  Result putWatering(const json& j);
  Result putCircuit(const json& j);
  Result deleteCircuit(const std::string& id);
  Result waterNow(const std::string& id, const json& j);
  Result stopWater(const std::string& id);
  Result pauseCircuit(const std::string& id, const json& j);
  // Light (SD-135): a circuit (a new one without an ID) and its removal; switching by hand {on:
  // true | false | null}, null back to the plan
  Result putLight(const json& j);
  Result deleteLight(const std::string& id);
  Result lightHand(const std::string& id, const json& j);
  // Climate (SD-136): a room added or changed, or removed with its outputs and sensors
  Result putRoom(const json& j);
  Result deleteRoom(const std::string& id);

  // ---- Access tokens (SD-129): only the owner manages them
  json tokens() const;
  Result createToken(const json& j);
  Result revokeToken(const std::string& id);

  // ---- Reading
  json info() const;
  json state();
  Epoch now() const { return clock_.epoch(); }
  json configJson() const;
  json history(const std::string& series, Epoch from, Epoch to, size_t points) const;
  // The history as CSV for a spreadsheet: a row per time in UTC, a column
  // per series. German ("de") with semicolons and a decimal comma, else
  // commas and a decimal point; a gap stays an empty field (RAT-016).
  std::string historyCsv(const std::vector<std::string>& series, Epoch from, Epoch to, const std::string& lang) const;
  json events(Epoch from, Epoch to, const std::string& type, size_t limit) const;
  static json templates();
  // The package of "Report a problem": version, settings, states and the
  // events of the last 72 hours, with a report number. No secrets: no
  // password hash, no sessions, no addresses; the app shows it first.
  json diagnostics();
  const MixCycle& cycle() const { return cycle_; }
  const SensorTruth& truth() const { return truth_; }

  // ---- Settings
  Result setSystem(const json& j);
  // Quiet hours, as local minutes of the day {"quietFromMin", "quietToMin"} (SD-123)
  Result putNotify(const json& j);
  // Maintenance dates (SD-125): the plan, an item done by hand, the intervals {"days": {kind: n | null}}
  // Plants and plant recipes (SD-128)
  // Commissioning (SD-129): each step of #10 with its state, and the next open one. A step the
  // robot sees is done while it sees it; the user confirms or skips the others.
  json setup() const;
  Result setSetup(const std::string& id, const json& j);
  json plants() const;
  // What lies ahead in the next `days` days, by local day: the plants' phases and harvests and
  // the maintenance dates (SD-128, SD-125)
  json calendar(int days) const;
  Result putPlantRecipe(const json& j);
  Result deletePlantRecipe(const std::string& id);
  Result startPlant(const json& j);
  Result putPlant(const std::string& id, const json& j);
  Result plantNext(const std::string& id);
  Result plantHarvest(const std::string& id, const json& j);
  Result plantComplete(const std::string& id);
  Result plantLead(const std::string& id);
  Result deletePlant(const std::string& id);
  json service() const;
  Result serviceDone(const std::string& id);
  Result putService(const json& j);
  Result putTank(const json& j);
  Result putControl(const json& j);
  // Which locks a person releases only report (SD-133): {reportOnly: [kind]}; each change is an event
  Result putLocks(const json& j);
  Result newTank();
  Result putCanister(const json& j);
  Result deleteCanister(const std::string& id);
  // A canister's level by hand: {"ml": n} as measured, or {"full": true} for
  // its size after a refill (SD-119)
  Result setStock(const std::string& id, const json& j);
  // A person ends the catch-up of a pair's missing parts (SD-114)
  Result endCatchUp();
  Result putRecipe(const json& j);
  Result deleteRecipe(const std::string& id);
  Result setActive(const json& j);
  // Restores a backup (a configuration as GET /config gives it): checked like
  // every change and its calibrations like a new one; refused while a dose,
  // a pump run or a probe calibration goes on. Never takes passwordSet from
  // the file. What the robot learned stays only where the file keeps it.
  Result importConfig(const json& j);

  // ---- Operation
  Result prime(int channel, double seconds);
  // A rate measured elsewhere, and whether the line is filled: the simulator's demo starts as a device
  // in use; not in the API
  Result setChannelFlow(int channel, double flowMlPerMin, bool lineFilled = false);
  Result cancelChannelJob();
  // A pump calibration by the user (SD-143): {"method": "cylinder" | "scale"}; the person's next step
  // ({"reading": ml or g} where a reading is wanted); cancelled by the person
  Result startPumpCalibration(int channel, const json& body);
  // Takes ({"take": true}) or dismisses the rate the consumption check proposed (SD-145)
  Result answerRateProposal(int channel, const json& body);
  Result pumpCalibrationNext(const json& body);
  Result cancelPumpCalibration();
  Result probe(const json& j);
  Result ackLatch(const std::string& id, const json& body);
  // Pauses the repetition of a raised alarm, or of all ("") (RAT-022, SD-123)
  Result ackAlarm(const std::string& id);
  // The notifications of late, newest first (SD-123)
  json notifications() const;
  // ---- Push channels (SD-124)
  // Their settings without secrets, and how each fared last
  json pushSettings() const;
  // MQTT with Home Assistant discovery (PD-461, SD-130): its settings without the password, and
  // the platform's report of the connection
  json mqttSettings() const;
  Result putMqtt(const json& j);
  const MqttSettings& mqtt() const { return mqtt_; }
  // Counts each change of the settings: the platform connects anew when it moves
  int mqttRevision() const { return mqttRevision_; }
  void setMqttStatus(bool connected, const Msg& error);
  // The MQTT topics roles read (SD-132), and a message on one of them
  std::vector<std::string> mqttValueTopics() const;
  void mqttValue(const std::string& topic, const std::string& payload);
  // What goes to the broker: the discovery configs and the state
  std::vector<MqttMessage> mqttDiscoveryMessages();
  MqttMessage mqttStateMessage();

  Result putPush(const json& j);
  // A test message to every channel set up, or to one
  Result testPush(const std::string& channel);
  // The platform takes what is to be sent and reports how it went
  std::vector<Delivery> takeDeliveries();
  void deliveryResult(const std::string& channel, bool ok, const std::string& error);
  Result stop(const Msg& who);
  Result resume();
  // Maintenance mode for `minutes` (at most kMaxMaintenanceMin; 0 ends it): the
  // cycle holds, the feed pump is off, jumps of the probes are taken as the work
  // and the watchdog does not judge the targets. Locks stay. In memory only:
  // a restart ends it.
  Result maintenance(double minutes);
  static constexpr double kMaxMaintenanceMin = 240;

  // An event from the API (a sign-in, for example), logged for security
  void logEvent(const std::string& type, const std::string& sev, Msg title, Msg text = {});

 private:
  Ctx ctx();
  bool flowOk() const;
  bool pumpMovesWater() const;
  bool ampsLow() const;
  Result saveConfig(Config next, const std::optional<Msg>& what);
  void saveState(bool force = false);
  void saveEvents();
  void sampleHistory(Epoch epoch);
  void watchRelease(Epoch epoch);
  std::string faultHold(const std::vector<std::string>& faults) const;
  void applyReturnPump();
  void checkHeads(Epoch epoch);
  void mixFirstNow();
  void learnSettling(const MixCycle::Settled& st, Epoch epoch);
  void snapshot(Epoch epoch);
  void channelJobDone(const DoseProgress& p);
  void pumpCalDone(Ctx& c, const DoseProgress& p);
  void takePumpCalResult();
  Result startChannelJob(const std::string& type, int channel, double seconds);
  bool channelJobActive() const { return job_ && job_->state == "running"; }
  void restartCycle();
  void logLockChanges(const Config& was);
  // Plants (SD-128): saves `next`, says what changed and starts the check anew when the water plan
  // changed; plants whose phase ended move on first
  Result savePlants(Config next, const Msg& what);
  void movePlants(Epoch epoch);
  // The water plan's EC target fell by more than 0.2 mS/cm: the advice to drain, until the tank fits
  // the new target (RAT-065, SD-140)
  void watchDrain(Epoch epoch);
  Msg drainAdvice() const;

  mutable std::recursive_mutex mtx_;
  IHardware& hw_;
  SealableStorage store_;  // sealed by a factory reset once cleared (SD-139)
  HubClock clock_;
  RandomFn rng_;
  json platform_ = json::object();

  Config cfg_;
  RuntimeState rt_;
  Auth auth_;
  SensorTruth truth_;
  Actuators act_;
  Doser doser_;
  DoseJournal journal_;
  MixCycle cycle_;
  History history_;
  EventLog log_;
  WatchResult watch_;
  WatchResult alarmWatch_;  // the watchdog's items and the maintenance items due, for the alarms
  Alarms alarms_;
  PushChannels push_;
  std::optional<Peer> peer_;
  std::optional<PairingCode> pairing_;
  std::vector<PeerRequest> peerOut_;
  struct PeerPending {
    std::string kind, address, tokenId, job;
  };
  std::map<std::string, RemoteResult> remoteRuns_;  // job → its state there: running until it ended
  bool leaderMaintenance_ = false;                  // on the follower: the leader is in maintenance
  Ms deviatesSince_ = -1;                           // on the leader: pH or EC differ from the follower's
  bool deviation_ = false;
  std::map<long long, PeerPending> peerPending_;
  long long peerSeq_ = 0;
  json pairStatus_ = nullptr;  // the last pairing from here: {state, address, error, at}
  // The link (SD-131). On the follower: when the leader last called, whether its pump runs, the
  // run asked for. On the leader: when the follower last answered, what it said, the last call.
  Ms peerHeard_ = -1;
  bool leaderPump_ = false;
  bool linkLost_ = false;
  json peerRun_ = nullptr;
  json peerState_ = nullptr;
  Ms beatSent_ = -1;
  bool peerLinkOk(Ms now) const { return peerHeard_ >= 0 && now - peerHeard_ <= kPeerLinkMs; }
  void watchPeer(Ctx& c);
  void watchDeviation(const Ctx& c);
  bool savePeer();
  MqttSettings mqtt_;
  NetDevices devices_;
  NetOutputs outputs_;
  RefillValve refill_;
  RefillIn refillIn(Ms now) const;
  // A fill in which the valve was open ended: its water is mixed in first, and the readings may jump
  void afterFill();
  // A fill ended: the check starts anew, after the water was mixed in where the valve was open
  void fillEnded(bool opened);
  bool circulationWanted(const Ctx& c) const;
  Watering water_;
  Lighting light_;
  Climate climate_;
  int secondOfDay(Epoch epoch) const;  // local
  Epoch waterChangedAt_ = 0;  // the last dose or fill: the water is judged by a check after it
  WaterIn waterIn(const Ctx& c) const;
  bool saveDeviceSecrets();
  void reconcileDevices(const Config& before);
  // The roles' states for the watchdog: each bound role, whether it works, and why not
  std::vector<WatchInput::Role> roleStates(Ms now) const;
  int mqttRevision_ = 0;
  json mqttStatus_ = {{"connected", false}, {"error", nullptr}, {"at", 0}};
  std::vector<Delivery> deliveries_;
  long long deliveryId_ = 1;
  json pushStatus_ = json::object();  // channel → {at, ok, error}, in memory
  static constexpr size_t kMaxDeliveries = 100;
  void queueDeliveries(std::vector<Delivery> ds);
  std::optional<ChannelJob> job_;
  PumpCal pumpCal_;
  // A probe calibration: the kind, its points and when the last step was
  // taken. It holds the cycle until it is saved or cancelled; it explains
  // jumps for 30 min after its last step (RAT-042).
  struct ProbeSession {
    std::string kind;  // ph | ec
    json points = json::array();
    double tempC = kNaN;
    Ms lastStep = 0;
  };
  std::optional<ProbeSession> probe_;
  bool authKept_ = false;
  bool restartWanted_ = false;  // a factory reset is done (SD-139)
  // A factory reset under way (SD-139): outputs go off, confirmed or for a minute, then the files go
  struct Resetting {
    std::string by;
    Ms until = 0;
  };
  std::optional<Resetting> resetting_;
  bool showCodes_ = false;  // the platform shows setup codes
  std::string codeToShow_;
  void armShownCode();
  void openKeyWindow(Ms now);
  void closeKeyWindow();
  void showStatusLight();
  Ms keyUntil_ = 0;
  int keyWrong_ = 0;
  void tickReset(Ms now);
  void finishFactoryReset();
  std::string deviceId_;

  Ms maintenanceUntilMs_ = 0;  // monotonic, so no clock step stretches it; 0: none

  bool stopButtonSeen_ = false;  // the stop button was pressed; a new press stops again (SD-110)
  std::set<std::string> faultsSeen_;  // the faults the device reported in the last tick (SD-141)
  std::map<std::string, Ms> faultLogged_;  // when each fault was last reported anew

  std::string lastHold_;         // the hold of the last tick: when it ends, the cycle starts anew
  bool lastChannelJob_ = false;  // a channel job ran in the last tick: when it ends, the check starts anew
  // The feed pump runs since then, its current is below water's since then, and it moved water in
  // this run (SD-146)
  std::optional<Ms> feedOnSince_, lowAmpsSince_;
  bool movedWater_ = false;
  Ms returnSince_ = -1;          // the return pump runs since then without a pause; -1: it stands (SD-109)
  Epoch maintenanceUntil() const;

  Epoch bootEpoch_ = 0;
  Ms bootMs_ = 0;
  Epoch lastSample_ = 0, lastWatch_ = 0, lastStateSave_ = 0, lastSnapshot_ = 0;
  std::string savedState_;
  std::uint64_t savedEventId_ = 0;
  int idSeq_ = 0;
};

}  // namespace doseph
