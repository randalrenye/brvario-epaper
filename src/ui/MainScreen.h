#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "config/DashboardLayoutConfig.h"
#include "config/ThermalAssistConfig.h"
#include "data/VarioData.h"
#include "display/EpdDisplay.h"
#include "navigation/ThermalCycleBeta.h"
#include "ui/Layout.h"
#include "ui/MapPage.h"
#include "ui/ThermalCyclePage.h"
#include "ui/TouchLayer.h"
#include "widgets/HeaderWidget.h"
#include "widgets/InfoGridWidget.h"
#include "widgets/SpeedGaugeWidget.h"
#include "widgets/VarioGaugeWidget.h"

class VarioBuzzer;
class FirmwareUpdater;
class WifiManager;
class MapDownloadManager;
class OpenWeatherClient;
class FlightSiteCatalogUpdater;
class PilotProfileConfig;
class FlightRecorder;
class TracklogBleService;
class StorageManager;
class WeatherLocationManager;
struct FlightSite;

class MainScreen {
 public:
  struct PageDebug {
    uint8_t page = 0;
    size_t zoneCount = 0;
  };

  explicit MainScreen(EpdDisplay& display);

  void attachAudioEditor(VarioBuzzer* buzzer);
  void attachFirmwareUpdater(FirmwareUpdater* updater);
  void attachWifiManager(WifiManager* wifi);
  void attachMapDownloadManager(MapDownloadManager* maps);
  void attachOpenWeatherClient(OpenWeatherClient* weather);
  void attachWeatherLocationManager(WeatherLocationManager* location);
  void attachFlightSiteCatalogUpdater(FlightSiteCatalogUpdater* updater);
  void attachPilotProfile(PilotProfileConfig* profile);
  void attachFlightRecorder(FlightRecorder* recorder);
  void attachTracklogBleService(TracklogBleService* ble);
  void attachThermalAssistConfig(ThermalAssistConfig* config);
  void attachStorageManager(StorageManager* storage);
  void begin(const VarioData& data);
  void update(const VarioData& data);
  void showDashboard(const VarioData& data);
  void showWeatherStationSleep(const VarioData& data);
  void setWeatherStationSleepMode(bool enabled);
  bool handleTouch(int32_t x, int32_t y);
  bool handleTouchHold(int32_t x, int32_t y);
  TouchAction lastTouchAction() const { return lastTouchAction_; }
  bool pageSwipePending() const { return pageSwipeActive_ && !pageSwipeConsumed_; }
  bool isDashboard() const { return activePage_ == Page::Dashboard; }
  bool isMapPage() const { return activePage_ == Page::Map; }
  bool isFlightDisplay() const { return activePage_ == Page::Dashboard || activePage_ == Page::Map; }
  bool keepsStationaryRuntimeActive() const { return activePage_ == Page::ThermalCycleBeta; }
  bool keepsWifiRuntimeActive() const {
    return activePage_ == Page::WifiSettings || activePage_ == Page::FirmwareUpdate || activePage_ == Page::MapDownload;
  }
  bool startupLocked() const { return dashboardStartupLock_; }
  TouchAction previewTouchAction(int32_t x, int32_t y) const;
  PageDebug currentPageDebug() const;

 private:
  static constexpr size_t kMaxTouchZones = 64;

  struct VisualState {
    uint16_t clockMinute = 0;
    uint8_t batteryPercent = 0;
    uint8_t satellites = 0;
    bool gpsFix = false;
    bool wifiEnabled = false;
    bool bluetoothActive = false;
    bool bluetoothConnected = false;
    bool batteryCharging = false;
    int16_t varioTenths = 0;
    int32_t ganhoM = 0;
    int16_t glideTenths = 0;
    uint8_t thermalCoreConfidencePercent = 0;
    bool thermalCoreMetricVisible = false;
    uint32_t elapsedSeconds = 0;
    int32_t altGpsM = 0;
    int32_t altAglM = 0;
    uint32_t thermalHash = 0;
    int32_t groundSpeedKmh = 0;
    int32_t windSpeedKmh = 0;
    int16_t courseDeg = 0;
    int16_t windDirectionDeg = 0;
    uint8_t windQuality = 0;
    bool audioEnabled = false;
    bool trackingEnabled = false;
  };

  enum class Page : uint8_t {
    Dashboard,
    Settings,
    Map,
    Customize,
    Tracklog,
    TracklogDetails,
    AudioEditor,
    DashboardLayout,
    WifiSettings,
    FirmwareUpdate,
    WeatherStation,
    WeatherLocation,
    PilotProfile,
    ThermalCycleBeta,
    ThermalAssistSettings,
    DeviceInfo,
    SystemStatus,
    Manual,
    ManualLogic,
    ManualUser,
    Storage,
    AdvancedSystem,
    MapDownload,
  };

  EpdDisplay& display_;
  LayoutRegions layout_;
  DashboardLayoutConfig dashboardLayout_;
  HeaderWidget header_;
  VarioGaugeWidget vario_;
  InfoGridWidget info_;
  SpeedGaugeWidget speed_;
  MapPage mapPage_;
  ThermalCycleBeta thermalCycleBeta_;
  ThermalCyclePage thermalCyclePage_;
  TouchZone touchZones_[kMaxTouchZones] = {};
  size_t touchZoneCount_ = 0;
  TouchAction lastTouchAction_ = TouchAction::None;
  uint16_t partialCycleCount_ = 0;
  uint32_t powerPressStartedMs_ = 0;
  uint32_t powerLastTouchMs_ = 0;
  bool powerConfirmVisible_ = false;
  bool pageSwipeActive_ = false;
  bool pageSwipeConsumed_ = false;
  int32_t pageSwipeStartX_ = 0;
  int32_t pageSwipeStartY_ = 0;
  Page activePage_ = Page::Dashboard;
  VarioBuzzer* audioBuzzer_ = nullptr;
  FirmwareUpdater* firmwareUpdater_ = nullptr;
  WifiManager* wifiManager_ = nullptr;
  MapDownloadManager* mapDownloadManager_ = nullptr;
  OpenWeatherClient* weatherClient_ = nullptr;
  WeatherLocationManager* weatherLocationManager_ = nullptr;
  FlightSiteCatalogUpdater* flightSiteCatalogUpdater_ = nullptr;
  PilotProfileConfig* pilotProfile_ = nullptr;
  FlightRecorder* flightRecorder_ = nullptr;
  TracklogBleService* tracklogBle_ = nullptr;
  ThermalAssistConfig* thermalAssistConfig_ = nullptr;
  StorageManager* storageManager_ = nullptr;
  bool audioSavedNotice_ = false;
  bool audioProfileDirty_ = false;
  uint8_t wifiKeyboardMode_ = 0;
  uint8_t profileKeyboardMode_ = 1;
  uint8_t profileSelectedField_ = 0;
  String wifiSelectedSsid_;
  String wifiPassword_;
  uint32_t wifiLastRefreshMs_ = 0;
  uint32_t keyboardSpaceLastMs_ = 0;
  uint8_t wifiLastState_ = 255;
  bool profileSavedNotice_ = false;
  bool profileDirty_ = false;
  uint16_t tracklogPage_ = 0;
  char selectedTracklogPath_[64] = {};
  char tracklogNotice_[56] = {};
  bool tracklogDeleteConfirm_ = false;
  bool tracklogBleStatusVisible_ = false;
  uint8_t tracklogBleLastProgress_ = 255;
  uint8_t mapDownloadLastProgress_ = 255;
  uint8_t mapDownloadLastState_ = 255;
  bool weatherStationSleepMode_ = false;
  bool weatherInfoPopupVisible_ = false;
  uint8_t weatherInfoScrollPage_ = 0;
  uint8_t weatherDayIndex_ = 0;
  enum class WeatherLocationView : uint8_t {
    Menu,
    Favorites,
    CatalogLoading,
    Catalog,
    CatalogSearch,
    CatalogStates,
    Manual,
  };
  WeatherLocationView weatherLocationView_ = WeatherLocationView::Menu;
  uint16_t weatherCatalogPage_ = 0;
  uint16_t weatherCatalogMatches_[512] = {};
  uint16_t weatherCatalogMatchCount_ = 0;
  bool weatherCatalogFilterDirty_ = true;
  uint16_t weatherCatalogScanIndex_ = 0;
  uint16_t weatherCatalogScanTotal_ = 0;
  uint8_t weatherCatalogLoadingProgress_ = 0;
  uint8_t weatherCatalogRenderedProgress_ = 0;
  char weatherCatalogQuery_[28] = {};
  char weatherCatalogState_[3] = {};
  uint8_t weatherCatalogLastProgress_ = 255;
  uint8_t weatherCatalogLastState_ = 255;
  uint8_t weatherManualField_ = 0;
  char weatherManualLatitude_[16] = {};
  char weatherManualLongitude_[16] = {};
  char weatherLocationNotice_[64] = {};
  bool weatherLocationSelectionRequired_ = false;
  bool thermalInfoPopupVisible_ = false;
  uint8_t thermalInfoScrollPage_ = 0;
  bool thermalCycleInfoPopupVisible_ = false;
  uint8_t thermalCycleInfoScrollPage_ = 0;
  bool dashboardLayoutSavedNotice_ = false;
  uint8_t manualPage_ = 0;
  uint8_t manualScrollOffset_ = 0;
  bool dashboardLayoutDragActive_ = false;
  DashboardWidgetKind dashboardLayoutDragWidget_ = DashboardWidgetKind::Vario;
  bool storageClearConfirm_ = false;
  TouchAction advancedConfirmAction_ = TouchAction::None;
  char advancedNotice_[80] = {};
  bool dashboardStartupLock_ = false;
  uint8_t dashboardStartupProgress_ = 0;
  uint32_t dashboardStartupStartedMs_ = 0;
  uint32_t dashboardStartupLastRefreshMs_ = 0;
  VarioData lastData_ = {};
  VisualState visualState_ = {};
  bool visualStateValid_ = false;
  uint32_t pageAutoRefreshMs_ = 0;
  uint32_t pageFullRefreshMs_ = 0;
  uint32_t mapDynamicRefreshMs_ = 0;
  uint32_t mapBasePrepareMs_ = 0;
  uint32_t footerDisplayedSignature_ = 0;
  uint32_t footerLastSoftCleanMs_ = 0;
  bool footerDisplayedSignatureValid_ = false;

  void renderStatic(const VarioData& data);
  void renderDynamic(const VarioData& data);
  void updateDynamicAreas(const VisualState& nextState);
  Rect_t lowerBandBounds() const;
  Rect_t footerContrastBounds() const;
  uint32_t footerSignature(Page page) const;
  bool footerNeedsPanelUpdate(Page page) const;
  void markFooterDisplayed(Page page);
  void renderActiveFooter();
  bool serviceFooterMaintenance(uint32_t now);
  Rect_t centerMetricBounds() const;
  void renderCenterMetrics(const VarioData& data);
  VisualState buildVisualState(const VarioData& data) const;
  void renderLayoutFrame();
  void renderFooterDynamic(const VarioData& data);
  void renderDashboardOverlays(const VarioData& data);
  void renderPowerConfirmPopup();
  void renderStartupLoadingPopup(const VarioData& data);
  void renderGpsNoFixPopup(const VarioData& data);
  void refreshDashboardOverlayArea(const Rect_t& area);
  Rect_t footerDynamicBounds() const;
  void openDashboard();
  void openPage(Page page);
  void refreshPageTransition(bool fullRefresh = false, bool reinforce = false);
  bool refreshIdlePageFullIfDue(uint32_t now);
  bool pageUsesFooterSettings(Page page) const;
  bool shouldQuickRefreshTransition(Page previousPage, Page nextPage) const;
  void renderPage(Page page);
  void syncMapPageData(const VarioData& data);
  void renderMapPageBase();
  void renderMapPageDynamic(bool renderFooter);
  void refreshMapPage(bool forceBase, bool fullRefresh = false);
  void prepareMapPageBaseCache(bool force = false);
  void renderPageFooter(Page page);
  void renderAudioEditorPage();
  void renderDashboardLayoutPage();
  void renderFirmwareUpdatePage();
  void renderWeatherStationPage();
  void renderWeatherInfoPopup();
  void renderWeatherLocationPage();
  void renderWifiSettingsPage();
  void renderPilotProfilePage();
  void renderThermalCycleBetaPage();
  void drawWifiPasswordValue();
  void refreshWifiPasswordValue();
  void drawPilotProfileFieldValue(uint8_t index);
  void refreshPilotProfileFieldValue(uint8_t index);
  void savePilotProfileIfDirty();
  void saveAudioProfileIfDirty();
  void refreshActivePageArea(const Rect_t& area);
  void refreshActivePageAreas(const Rect_t* areas, size_t count, bool updateTouchZones = false);
  void refreshAudioEditorControls();
  void refreshWifiStatusArea();
  void refreshWifiKeyboardArea();
  void refreshPilotProfileKeyboardArea();
  void refreshPilotProfileSelection(uint8_t previousIndex, uint8_t nextIndex);
  void renderThermalAssistSettingsPage();
  void renderThermalInfoPopup();
  void renderThermalCycleInfoPopup();
  void renderDeviceInfoPage();
  void renderSystemStatusPage();
  void renderManualPage();
  void renderManualTopicPage(bool userManual);
  void renderManualLogicPage();
  void renderManualUserPage();
  void renderStoragePage();
  void renderAdvancedSystemPage();
  void renderMapDownloadPage();
  void renderTracklogPage();
  void renderTracklogDetailsPage();
  void renderTracklogBleStatusBox();
  void refreshActivePage(bool fullRefresh = false);
  void registerTouchZones();
  void registerPageTouchZones();
  void addTouchZone(const Rect_t& bounds, TouchAction action);
  bool dispatchTouchAction(TouchAction action);
  TouchAction actionAt(int32_t x, int32_t y) const;
  TouchAction footerActionAt(int32_t x, int32_t y) const;
  Rect_t dashboardSlotBounds(DashboardSlot slot) const;
  Rect_t dashboardLayoutSlotBounds(DashboardSlot slot) const;
  Rect_t dashboardLayoutPresetBounds(DashboardWidgetKind widget) const;
  Rect_t dashboardLayoutSaveButtonBounds() const;
  Rect_t dashboardLayoutResetButtonBounds() const;
  bool dashboardSlotAt(int32_t x, int32_t y, DashboardSlot& slot) const;
  bool dashboardPresetAt(int32_t x, int32_t y, DashboardWidgetKind& widget) const;
  bool handleDashboardLayoutTouch(int32_t x, int32_t y);
  bool handleFirmwareUpdateTouch(int32_t x, int32_t y);
  bool handleWifiTouch(int32_t x, int32_t y);
  bool handlePilotProfileTouch(int32_t x, int32_t y);
  bool handleTracklogTouch(int32_t x, int32_t y);
  bool handleTracklogDetailsTouch(int32_t x, int32_t y);
  bool handleTracklogBleStatusTouch(int32_t x, int32_t y);
  bool handleAudioEditorTouch(int32_t x, int32_t y);
  bool handleWeatherLocationTouch(int32_t x, int32_t y);
  bool previewWeatherLocationTouch(int32_t x, int32_t y) const;
  void beginWeatherCatalogMatchRebuild();
  bool processWeatherCatalogMatchChunk(uint16_t maxSites);
  const FlightSite* weatherCatalogFilteredSite(uint16_t index);
  bool appendWeatherCatalogSearchChar(char value);
  bool weatherHasActiveForecast() const;
  void requestWeatherForActiveLocation();
  void prepareManualWeatherCoordinates();
  bool appendManualCoordinateChar(char value);
  bool saveManualWeatherCoordinates();
  void setAdvancedNotice(const char* text);
  void requestAdvancedConfirmation(TouchAction action, const char* notice);
  void executeAdvancedAction(TouchAction action);
  bool resetSavedRuntimeSettings(bool includeWifi);
  Rect_t settingsThermalAssistButtonBounds() const;
  Rect_t settingsThermalAssistTouchBounds() const;
  Rect_t settingsDeviceInfoButtonBounds() const;
  Rect_t settingsDeviceInfoTouchBounds() const;
  Rect_t settingsSystemStatusButtonBounds() const;
  Rect_t settingsSystemStatusTouchBounds() const;
  Rect_t settingsManualButtonBounds() const;
  Rect_t settingsManualTouchBounds() const;
  Rect_t manualLogicButtonBounds() const;
  Rect_t manualUserButtonBounds() const;
  Rect_t settingsStorageButtonBounds() const;
  Rect_t settingsStorageTouchBounds() const;
  Rect_t settingsAdvancedSystemButtonBounds() const;
  Rect_t settingsAdvancedSystemTouchBounds() const;
  Rect_t settingsWifiButtonBounds() const;
  Rect_t settingsWifiTouchBounds() const;
  Rect_t settingsFirmwareUpdateButtonBounds() const;
  Rect_t settingsFirmwareUpdateTouchBounds() const;
  Rect_t settingsWeatherStationButtonBounds() const;
  Rect_t settingsWeatherStationTouchBounds() const;
  Rect_t settingsPilotProfileButtonBounds() const;
  Rect_t settingsPilotProfileTouchBounds() const;
  Rect_t settingsThermalCycleButtonBounds() const;
  Rect_t settingsThermalCycleTouchBounds() const;
  Rect_t firmwareUpdateStartButtonBounds() const;
  Rect_t firmwareUpdateWifiButtonBounds() const;
  Rect_t wifiScanButtonBounds() const;
  Rect_t wifiConnectButtonBounds() const;
  Rect_t wifiClearButtonBounds() const;
  Rect_t wifiNetworkRowBounds(uint8_t index) const;
  Rect_t wifiStatusAreaBounds() const;
  Rect_t wifiKeyboardAreaBounds() const;
  Rect_t wifiKeyBounds(uint8_t row, uint8_t col) const;
  Rect_t wifiSpecialKeyBounds(uint8_t index) const;
  char wifiKeyAt(uint8_t row, uint8_t col) const;
  Rect_t profileFieldBounds(uint8_t index) const;
  Rect_t profileKeyboardAreaBounds() const;
  Rect_t profileKeyBounds(uint8_t row, uint8_t col) const;
  Rect_t profileSpecialKeyBounds(uint8_t index) const;
  char profileKeyAt(uint8_t row, uint8_t col) const;
  Rect_t tracklogPrevButtonBounds() const;
  Rect_t tracklogNextButtonBounds() const;
  Rect_t tracklogSyncButtonBounds() const;
  Rect_t tracklogRowBounds(uint8_t index) const;
  Rect_t tracklogRowDeleteButtonBounds(uint8_t index) const;
  Rect_t tracklogDeleteButtonBounds() const;
  Rect_t tracklogExportButtonBounds() const;
  Rect_t tracklogInfoButtonBounds() const;
  Rect_t tracklogConfirmPopupBounds() const;
  Rect_t tracklogConfirmYesButtonBounds() const;
  Rect_t tracklogConfirmNoButtonBounds() const;
  Rect_t tracklogBleStatusPopupBounds() const;
  Rect_t tracklogBleCancelButtonBounds() const;
  Rect_t powerConfirmPopupBounds() const;
  Rect_t powerConfirmYesButtonBounds() const;
  Rect_t powerConfirmNoButtonBounds() const;
  Rect_t startupLoadingPopupBounds() const;
  Rect_t gpsNoFixPopupBounds() const;
  Rect_t weatherPrevDayButtonBounds() const;
  Rect_t weatherNextDayButtonBounds() const;
  Rect_t weatherInfoButtonBounds() const;
  Rect_t weatherInfoPopupBounds() const;
  Rect_t weatherInfoCloseButtonBounds() const;
  Rect_t weatherInfoScrollUpButtonBounds() const;
  Rect_t weatherInfoScrollDownButtonBounds() const;
  Rect_t weatherLocationLabelBounds() const;
  Rect_t weatherLocationEnterButtonBounds() const;
  Rect_t weatherLocationMenuButtonBounds(uint8_t index) const;
  Rect_t weatherLocationRowBounds(uint8_t index) const;
  Rect_t weatherLocationRowActionBounds(uint8_t index) const;
  Rect_t weatherCatalogControlBounds(uint8_t index) const;
  Rect_t weatherCatalogSearchButtonBounds() const;
  Rect_t weatherCatalogStateButtonBounds() const;
  Rect_t weatherCatalogSearchFieldBounds() const;
  Rect_t weatherCatalogSearchKeyBounds(uint8_t row, uint8_t col) const;
  Rect_t weatherCatalogSearchSpecialBounds(uint8_t index) const;
  Rect_t weatherCatalogStateChoiceBounds(uint8_t index) const;
  Rect_t weatherManualFieldBounds(uint8_t index) const;
  Rect_t weatherManualKeyBounds(uint8_t row, uint8_t col) const;
  Rect_t weatherManualDeleteButtonBounds() const;
  Rect_t weatherManualSaveButtonBounds() const;
  Rect_t thermalInfoButtonBounds() const;
  Rect_t thermalInfoPopupBounds() const;
  Rect_t thermalInfoCloseButtonBounds() const;
  Rect_t thermalInfoScrollUpButtonBounds() const;
  Rect_t thermalInfoScrollDownButtonBounds() const;
  Rect_t thermalCycleInfoButtonBounds() const;
  Rect_t thermalCycleInfoPopupBounds() const;
  Rect_t thermalCycleInfoCloseButtonBounds() const;
  Rect_t thermalCycleInfoScrollUpButtonBounds() const;
  Rect_t thermalCycleInfoScrollDownButtonBounds() const;
  Rect_t manualTabBounds(uint8_t index) const;
  Rect_t manualScrollUpButtonBounds() const;
  Rect_t manualScrollDownButtonBounds() const;
  uint8_t manualActiveLineCount() const;
  Rect_t storageRefreshButtonBounds() const;
  Rect_t storageClearMapsButtonBounds() const;
  Rect_t storageDownloadButtonBounds() const;
  Rect_t advancedRecoverDisplayButtonBounds() const;
  Rect_t advancedMoveIgcButtonBounds() const;
  Rect_t advancedClearWifiButtonBounds() const;
  Rect_t advancedResetSettingsButtonBounds() const;
  Rect_t advancedClearWeatherButtonBounds() const;
  Rect_t advancedFormatSystemButtonBounds() const;
  Rect_t advancedConfirmYesButtonBounds() const;
  Rect_t advancedConfirmNoButtonBounds() const;
  Rect_t mapRegionButtonBounds(uint8_t index) const;
  Rect_t mapDownloadStartButtonBounds() const;
  Rect_t mapDownloadCancelButtonBounds() const;
  Rect_t mapZoomInButtonBounds() const;
  Rect_t mapZoomOutButtonBounds() const;
  Rect_t mapPanUpButtonBounds() const;
  Rect_t mapPanDownButtonBounds() const;
  Rect_t mapPanLeftButtonBounds() const;
  Rect_t mapPanRightButtonBounds() const;
  void openTracklogDetails(const char* filepath);
  void configureDashboardWidgetBounds();
  Rect_t settingsAudioEditorButtonBounds() const;
  Rect_t settingsAudioEditorTouchBounds() const;
  Rect_t settingsDashboardLayoutButtonBounds() const;
  Rect_t settingsDashboardLayoutTouchBounds() const;
  Rect_t audioAdjustButtonBounds(uint8_t row, bool plus) const;
  Rect_t audioSaveButtonBounds() const;
  Rect_t audioResetButtonBounds() const;
  Rect_t audioVoiceToggleButtonBounds() const;
  Rect_t audioVolumeSliderBounds() const;
  Rect_t audioVolumeSliderTouchBounds() const;
  Rect_t audioEditorControlsBounds() const;
  Rect_t thermalModeButtonBounds(ThermalAssistVisualMode mode) const;
  void drawButton(const Rect_t& bounds, const char* label, uint8_t scale = 2);
};
