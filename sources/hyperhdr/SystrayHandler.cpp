/* SystrayHandler.cpp
*
*  MIT License
*
*  Copyright (c) 2020-2026 awawa-dev
*
*  Project homesite: https://github.com/awawa-dev/HyperHDR
*
*  Permission is hereby granted, free of charge, to any person obtaining a copy
*  of this software and associated documentation files (the "Software"), to deal
*  in the Software without restriction, including without limitation the rights
*  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*  copies of the Software, and to permit persons to whom the Software is
*  furnished to do so, subject to the following conditions:
*
*  The above copyright notice and this permission notice shall be included in all
*  copies or substantial portions of the Software.

*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
*  SOFTWARE.
*/

#ifndef PCH_ENABLED
	#include <QSettings>
	#include <list>
#endif

#ifndef _WIN32
	#include <unistd.h>
#else
	#include <windows.h>
#endif

#ifdef __APPLE__
	#include <ApplicationServices/ApplicationServices.h>
#endif

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QSettings>

#include <HyperhdrConfig.h>

#include <image/ColorRgb.h>
#include <effects/EffectDefinition.h>
#include <webserver/WebServer.h>
#include <utils/Logger.h>
#include <systray/Systray.h>
#include <utils-image/utils-image.h>

#include "HyperHdrDaemon.h"
#include "SystrayHandler.h"
#include "BrightnessManager.h"

namespace
{
	// brightness levels offered by the tray menu
	const int BRIGHTNESS_LEVEL_MIN = 10;
	const int BRIGHTNESS_LEVEL_MAX = 100;
	const int BRIGHTNESS_LEVEL_STEP = 10;

	#ifdef _WIN32
		const int HOTKEY_BRIGHTNESS_UP = 1;
		const int HOTKEY_BRIGHTNESS_DOWN = 2;
		const int HOTKEY_NIGHT_MODE = 3;
	#endif
}

#ifdef __linux__
#define SYSTRAY_WIDGET_LIB "libsystray-widget.so"
#include <cstdlib>
#include <dlfcn.h>
namespace
{
	void* _library = nullptr;
	SystrayInitializeFun SystrayInitialize = nullptr;
	SystrayLoopFun SystrayLoop = nullptr;
	SystrayUpdateFun SystrayUpdate = nullptr;
	SystrayCloseFun SystrayClose = nullptr;
}
#endif

SystrayHandler::SystrayHandler(HyperHdrDaemon* hyperhdrDaemon, quint16 webPort, QString rootFolder)
	: QObject(),
	_menu(nullptr),
	_haveSystray(false),
	_webPort(webPort),
	_rootFolder(rootFolder),
	_selectedInstance(-1)
{
	Q_INIT_RESOURCE(resources);

	std::shared_ptr<HyperHdrManager> instanceManager;
	hyperhdrDaemon->getInstanceManager(instanceManager);
	_instanceManager = instanceManager;
	connect(instanceManager.get(), &HyperHdrManager::SignalInstanceStateChanged, this, &SystrayHandler::signalInstanceStateChangedHandler);
	connect(instanceManager.get(), &HyperHdrManager::SignalSettingsChanged, this, &SystrayHandler::signalSettingsChangedHandler);

	hyperhdrDaemon->getBrightnessManager(_brightnessManager);
	if (_brightnessManager != nullptr)
	{
		connect(_brightnessManager.get(), &BrightnessManager::SignalBrightnessChanged, this, &SystrayHandler::signalBrightnessChangedHandler);
		connect(_brightnessManager.get(), &BrightnessManager::SignalHotkeysChanged, this, &SystrayHandler::signalHotkeysChangedHandler);
	}

#ifdef __linux__
	// Load library
	_library = dlopen(SYSTRAY_WIDGET_LIB, RTLD_NOW);

	if (_library)
	{
		SystrayInitialize = (SystrayInitializeFun)dlsym(_library, "SystrayInitialize");
		SystrayLoop = (SystrayLoopFun)dlsym(_library, "SystrayLoop");
		SystrayUpdate = (SystrayUpdateFun)dlsym(_library, "SystrayUpdate");
		SystrayClose = (SystrayCloseFun)dlsym(_library, "SystrayClose");

		if (SystrayInitialize == nullptr || SystrayLoop == nullptr || SystrayUpdate == nullptr || SystrayClose == nullptr)
		{
			printf("Could not resolve libSystrayWidget.so functions\n");
			dlclose(_library);
			_library = nullptr;
		}
	}
	else
		printf("Could not load libSystrayWidget.so library\n");

	if (_library != nullptr)
		_haveSystray = SystrayInitialize(nullptr);
#else
	_haveSystray = SystrayInitialize(nullptr);
#endif	

#ifdef _WIN32
	if (_haveSystray)
	{
		SystrayAssignHotkeyHandler([this](int hotkeyId) {
			if (_brightnessManager == nullptr)
				return;

			switch (hotkeyId)
			{
				case HOTKEY_BRIGHTNESS_UP:
					QUEUE_CALL_0(_brightnessManager.get(), increaseBrightness);
					break;
				case HOTKEY_BRIGHTNESS_DOWN:
					QUEUE_CALL_0(_brightnessManager.get(), decreaseBrightness);
					break;
				case HOTKEY_NIGHT_MODE:
					QUEUE_CALL_0(_brightnessManager.get(), toggleNightMode);
					break;
			}
		});

		updateHotkeys();
	}
#endif
}

SystrayHandler::~SystrayHandler()
{
	printf("Releasing SysTray\n");
	if (_haveSystray)
		SystrayClose();

#ifdef __linux__
	if (_library != nullptr)
	{
		dlclose(_library);
		_library = nullptr;
	}
#endif
}

bool SystrayHandler::isInitialized()
{
	return _haveSystray;
}

void SystrayHandler::loop()
{
	if (_haveSystray)
		SystrayLoop();
}

void SystrayHandler::close()
{
	if (_haveSystray)
	{
		SystrayClose();
	}
}

static QString preloadSvg(const QString& filename)
{
	if (filename.indexOf(":/") == 0)
	{
		QFile stream(filename);
		if (!stream.open(QIODevice::ReadOnly))
			return filename;
		QByteArray ar = stream.readAll();
		stream.close();

		return QString(ar);
	}
	return filename;
}

static void loadSvg(std::unique_ptr<SystrayMenu>& menu, QString filename, [[maybe_unused]] QString rootFolder, [[maybe_unused]] QString destFilename = "")
{

#ifdef __linux__
			int iconDim = 16;
	#else
		#ifdef __APPLE__
			int iconDim = 18;
		#else
			int iconDim = 22;

			if (filename == ":/hyperhdr-tray-icon.svg")
				iconDim = 32;
	#endif
#endif

#ifdef __linux__
	if (destFilename.isEmpty())
	{
		destFilename = filename;
		if (destFilename.indexOf(":/") == 0)
			destFilename = destFilename.right(destFilename.size() - 2);
		destFilename.replace(".svg", ".png");
	}

	QString fullPath = rootFolder + "/systray-icons/" + destFilename;
	QFileInfo iconFile(fullPath);

	if (!iconFile.exists())
	{
		QDir().mkpath(iconFile.absolutePath());

		std::vector<uint8_t> ar;

		QString svgData = preloadSvg(filename);

		if (filename.contains("clear.svg", Qt::CaseInsensitive)) {			
			svgData.replace("fill=\"black\"", QString("fill=\"#FFFF00\""));
		}
		else if (filename.contains("settings.svg", Qt::CaseInsensitive)) {
			svgData.replace("fill=\"black\"", QString("fill=\"#00FF00\""));
		}

		utils_image::svg2png(svgData.toStdString(), iconDim, iconDim, ar);

		QFile newIcon(fullPath);
		newIcon.open(QIODevice::WriteOnly);
		newIcon.write(reinterpret_cast<char*>(ar.data()), ar.size());
		newIcon.close();
	}

	menu->tooltip = fullPath.toStdString();
#else
	std::vector<uint8_t> ar;
	QString svgFile = preloadSvg(filename);
	#ifdef __APPLE__
	if (filename.indexOf(":/") == 0 && SystrayDarkmode())
	{
		svgFile.replace("fill=\"black\"", "fill=\"white\"");
	}
	#endif
	utils_image::svg2png(svgFile.toStdString(), iconDim, iconDim, ar);

	menu->icon.resize(ar.size());
	memcpy(menu->icon.data(), ar.data(), ar.size());
#endif
}

void SystrayHandler::createSystray()
{
	if (!_haveSystray)
		return;

	std::unique_ptr<SystrayMenu> mainMenu = std::make_unique<SystrayMenu>();

	loadSvg(mainMenu, ":/hyperhdr-tray-icon.svg", _rootFolder);

	// settings menu
	std::unique_ptr<SystrayMenu> settingsMenu = std::make_unique<SystrayMenu>();
	loadSvg(settingsMenu, ":/settings.svg", _rootFolder);
	settingsMenu->label = "&Settings";
	settingsMenu->context = this;
	settingsMenu->callback = [](SystrayMenu* m) {
		SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
		if (sh != nullptr)
			QUEUE_CALL_0(sh, settings);
	};

	// separator 1
	std::unique_ptr<SystrayMenu> separator1 = std::make_unique<SystrayMenu>();
	separator1->label = "-";

	// instances
	std::unique_ptr<SystrayMenu> instances = std::make_unique<SystrayMenu>();
	instances->label = "Instances";
	loadSvg(instances, ":/instance.svg", _rootFolder);

	std::shared_ptr<HyperHdrManager> instanceManager = _instanceManager.lock();
	if (instanceManager != nullptr)
	{
		std::vector<QString> instancesList;
		SAFE_CALL_0_RET(instanceManager.get(), getInstances, std::vector<QString>, instancesList);
		for (int i = static_cast<int>(instancesList.size()) - 2; i >= 0; i -= 2)
		{
			bool ok = false;
			int key = instancesList[i].toInt(&ok);
			if (ok)
			{
				std::unique_ptr<SystrayMenu> instanceItem = std::make_unique<SystrayMenu>();
				instanceItem->label = instancesList[i + 1].toStdString();
				instanceItem->checkGroup = key;
				instanceItem->isChecked = (key == _selectedInstance);
				instanceItem->context = this;
				instanceItem->callback = [](SystrayMenu* m) {
					SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
					sh->_selectedInstance = m->checkGroup;
					if (sh != nullptr)
						QUEUE_CALL_0(sh, selectInstance);
				};

				std::swap(instances->submenu, instanceItem->next);
				std::swap(instances->submenu, instanceItem);
			}
		}

		std::unique_ptr<SystrayMenu> separatorInstance = std::make_unique<SystrayMenu>();
		separatorInstance->label = "-";

		std::swap(instances->submenu, separatorInstance->next);
		std::swap(instances->submenu, separatorInstance);

		std::unique_ptr<SystrayMenu> instancesAll = std::make_unique<SystrayMenu>();
		instancesAll->label = "All";
		instancesAll->isChecked = (_selectedInstance == -1);
		instancesAll->context = this;
		instancesAll->callback = [](SystrayMenu* m) {
			SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
			sh->_selectedInstance = -1;
			if (sh != nullptr)
				QUEUE_CALL_0(sh, selectInstance);
		};

		std::swap(instances->submenu, instancesAll->next);
		std::swap(instances->submenu, instancesAll);
	}
	
	// color menu
	std::unique_ptr<SystrayMenu> colorMenu = std::make_unique<SystrayMenu>();
	loadSvg(colorMenu, ":/color.svg", _rootFolder);
	colorMenu->label = "&Color";
	colorMenu->context = this;

	std::list<std::string> colors{ "white", "red", "green", "blue", "yellow", "magenta", "cyan" };
	colors.reverse();
	for (const std::string& color : colors)
	{
		QString svgTemplate = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"32\" height=\"32\" version=\"1.1\" viewBox=\"0 0 32 32\">"
			"<rect width=\"26\" height=\"26\" x=\"3\" y=\"3\" fill=\"%1\" stroke-width=\"2\" stroke=\"gray\" />"
			"</svg>";
		QString svg = QString(svgTemplate).arg(QString::fromStdString(color));
		
		std::unique_ptr<SystrayMenu> colorItem = std::make_unique<SystrayMenu>();
		loadSvg(colorItem, svg, _rootFolder, QString("%1.png").arg(QString::fromStdString(color)));
		colorItem->label = color;
		colorItem->context = this;
		colorItem->callback = [](SystrayMenu* m) {
			SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
			QString colorName = QString::fromStdString(m->label);
			ColorRgb color = utils_image::colorRgbfromString(colorName.toStdString());
			if (sh != nullptr)
				QUEUE_CALL_1(sh, setColor, ColorRgb, color);
		};

		std::swap(colorMenu->submenu, colorItem->next);
		std::swap(colorMenu->submenu, colorItem);
	}

	// effects
	std::list<EffectDefinition> efxs, efxsSorted;
	if (instanceManager != nullptr)
		efxs = instanceManager->getEffects();

	std::unique_ptr<SystrayMenu> effectsMenu = std::make_unique<SystrayMenu>();
	loadSvg(effectsMenu, ":/effects.svg", _rootFolder);
	effectsMenu->label = "&Effects";


	efxs.sort([](const EffectDefinition& a, const EffectDefinition& b) {
		return a.name > b.name;
	});

	std::copy_if(efxs.begin(), efxs.end(),
		std::back_inserter(efxsSorted),
		[](const EffectDefinition& a) { return a.name.find("Music:") != std::string::npos; });

	std::copy_if(efxs.begin(), efxs.end(),
		std::back_inserter(efxsSorted),
		[](const EffectDefinition& a) { return !(a.name.find("Music:") != std::string::npos); });


	for (const EffectDefinition& efx : efxsSorted)
	{
		QString effectName = QString::fromStdString(efx.name);
		std::unique_ptr<SystrayMenu> effectItem = std::make_unique<SystrayMenu>();
		effectItem->label = effectName.toStdString();
		effectItem->context = this;
		effectItem->callback = [](SystrayMenu* m) {
			SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
			QString effectName = QString::fromStdString(m->label);
			if (sh != nullptr)
				QUEUE_CALL_1(sh, setEffect, QString, effectName);
		};

		std::swap(effectsMenu->submenu, effectItem->next);
		std::swap(effectsMenu->submenu, effectItem);
	}

	// brightness menu
	std::unique_ptr<SystrayMenu> brightnessMenu = std::make_unique<SystrayMenu>();
	loadSvg(brightnessMenu, ":/brightness.svg", _rootFolder);
	brightnessMenu->label = "&Brightness";
	brightnessMenu->context = this;

	// night mode menu
	std::unique_ptr<SystrayMenu> nightModeMenu = std::make_unique<SystrayMenu>();
	loadSvg(nightModeMenu, ":/nightmode.svg", _rootFolder);
	nightModeMenu->label = "&Night mode";
	nightModeMenu->context = this;
	nightModeMenu->isDisabled = (_brightnessManager == nullptr);
	nightModeMenu->isChecked = (_brightnessManager != nullptr && _brightnessManager->isNightMode());
	nightModeMenu->callback = [](SystrayMenu* m) {
		SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
		if (sh != nullptr)
			QUEUE_CALL_0(sh, toggleNightMode);
	};

	if (_brightnessManager == nullptr)
		brightnessMenu->isDisabled = true;
	else
	{
		const int currentBrightness = _brightnessManager->getBrightness();

		for (int level = BRIGHTNESS_LEVEL_MIN; level <= BRIGHTNESS_LEVEL_MAX; level += BRIGHTNESS_LEVEL_STEP)
		{
			std::unique_ptr<SystrayMenu> brightnessItem = std::make_unique<SystrayMenu>();
			brightnessItem->label = std::to_string(level) + "%";
			brightnessItem->checkGroup = level;
			// the current value does not have to be a multiple of the step, so mark the closest level
			brightnessItem->isChecked = (currentBrightness > level - BRIGHTNESS_LEVEL_STEP && currentBrightness <= level) ||
										(level == BRIGHTNESS_LEVEL_MIN && currentBrightness <= level);
			brightnessItem->context = this;
			brightnessItem->callback = [](SystrayMenu* m) {
				SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
				int level = m->checkGroup;
				if (sh != nullptr)
					QUEUE_CALL_1(sh, setBrightness, int, level);
			};

			std::swap(brightnessMenu->submenu, brightnessItem->next);
			std::swap(brightnessMenu->submenu, brightnessItem);
		}
	}

	// clear menu
	std::unique_ptr<SystrayMenu> clearMenu = std::make_unique<SystrayMenu>();
	loadSvg(clearMenu, ":/clear.svg", _rootFolder);
	clearMenu->label = "&Clear";
	clearMenu->context = this;
	clearMenu->callback = [](SystrayMenu* m) {
		SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
		if (sh != nullptr)
			QUEUE_CALL_0(sh, clearEfxColor);
	};

	// separator 2
	std::unique_ptr<SystrayMenu> separator2 = std::make_unique<SystrayMenu>();
	separator2->label = "-";

	// quit menu
	std::unique_ptr<SystrayMenu> quitMenu = std::make_unique<SystrayMenu>();
	loadSvg(quitMenu, ":/quit.svg", _rootFolder);
	quitMenu->label = "&Quit";
	quitMenu->context = this;
	quitMenu->callback = [](SystrayMenu* m) {
		SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
		if (sh != nullptr)
		{
			auto instance = QCoreApplication::instance();
			QUEUE_CALL_0(instance, quit);
		}
	};


#ifdef _WIN32
	std::unique_ptr<SystrayMenu> separator3 = std::unique_ptr<SystrayMenu>(new SystrayMenu);
	separator3->label = "-";

	std::unique_ptr<SystrayMenu> autostartMenu = std::unique_ptr<SystrayMenu>(new SystrayMenu);
	loadSvg(autostartMenu, ":/autorun.svg", _rootFolder);
	autostartMenu->label = (getCurrentAutorunState()) ? "Disable autostart" : "Enable autostart";
	autostartMenu->context = this;
	autostartMenu->callback = [](SystrayMenu* m) {
		SystrayHandler* sh = qobject_cast<SystrayHandler*>(m->context);
		if (sh != nullptr)
		{
			QUEUE_CALL_0(sh, setAutorunState);
		}
	};
#endif

	std::swap(separator2->next, quitMenu);
	std::swap(clearMenu->next, separator2);
	std::swap(effectsMenu->next, clearMenu);
	std::swap(colorMenu->next, effectsMenu);
	std::swap(nightModeMenu->next, colorMenu);
	std::swap(brightnessMenu->next, nightModeMenu);

#ifdef _WIN32
	std::swap(instances->next, brightnessMenu);
	std::swap(separator3->next, instances);
	std::swap(autostartMenu->next, separator3);
	std::swap(separator1->next, autostartMenu);
#else
	std::swap(instances->next, brightnessMenu);
	std::swap(separator1->next, instances);
#endif

	std::swap(settingsMenu->next, separator1);	
	std::swap(mainMenu->submenu, settingsMenu);

	SystrayUpdate(mainMenu.get());
	std::swap(_menu, mainMenu);
}

#ifdef _WIN32
bool SystrayHandler::getCurrentAutorunState()
{	
	QSettings reg("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
	return reg.value("Hyperhdr", 0).toString() == qApp->applicationFilePath().replace('/', '\\');
}
#endif

void SystrayHandler::setAutorunState()
{
	#ifdef _WIN32
		bool currentState = getCurrentAutorunState();
		QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
		(currentState)
			? reg.remove("Hyperhdr")
			: reg.setValue("Hyperhdr", qApp->applicationFilePath().replace('/', '\\'));
		QUEUE_CALL_0(this, createSystray);
	#endif
}

void SystrayHandler::selectInstance()
{
	createSystray();
}

void SystrayHandler::setBrightness(int percent)
{
	if (_brightnessManager != nullptr)
		_brightnessManager->setBrightness(percent);
}

void SystrayHandler::toggleNightMode()
{
	if (_brightnessManager != nullptr)
		_brightnessManager->toggleNightMode();
}

void SystrayHandler::signalBrightnessChangedHandler(int /*brightness*/, bool /*nightMode*/)
{
	createSystray();
}

void SystrayHandler::signalHotkeysChangedHandler()
{
	#ifdef _WIN32
		updateHotkeys();
	#endif
}

#ifdef _WIN32
bool SystrayHandler::parseHotkey(const QString& definition, unsigned int& modifiers, unsigned int& virtualKey)
{
	modifiers = 0;
	virtualKey = 0;

	const QStringList tokens = definition.split('+', Qt::SkipEmptyParts);

	for (const QString& rawToken : tokens)
	{
		const QString token = rawToken.trimmed().toUpper();

		if (token.isEmpty())
			return false;
		else if (token == "CTRL" || token == "CONTROL")
			modifiers |= MOD_CONTROL;
		else if (token == "ALT")
			modifiers |= MOD_ALT;
		else if (token == "SHIFT")
			modifiers |= MOD_SHIFT;
		else if (token == "WIN" || token == "META")
			modifiers |= MOD_WIN;
		else if (virtualKey != 0)
			return false;
		else if (token == "UP")
			virtualKey = VK_UP;
		else if (token == "DOWN")
			virtualKey = VK_DOWN;
		else if (token == "LEFT")
			virtualKey = VK_LEFT;
		else if (token == "RIGHT")
			virtualKey = VK_RIGHT;
		else if (token == "PLUS" || token == "ADD")
			virtualKey = VK_ADD;
		else if (token == "MINUS" || token == "SUBTRACT")
			virtualKey = VK_SUBTRACT;
		else if (token == "PAGEUP")
			virtualKey = VK_PRIOR;
		else if (token == "PAGEDOWN")
			virtualKey = VK_NEXT;
		else if (token == "HOME")
			virtualKey = VK_HOME;
		else if (token == "END")
			virtualKey = VK_END;
		else if (token == "INSERT")
			virtualKey = VK_INSERT;
		else if (token == "DELETE")
			virtualKey = VK_DELETE;
		else if (token == "SPACE")
			virtualKey = VK_SPACE;
		else if (token.size() >= 2 && token.size() <= 3 && token.at(0) == QLatin1Char('F') && token.mid(1).toInt() >= 1 && token.mid(1).toInt() <= 24)
			virtualKey = VK_F1 + (token.mid(1).toInt() - 1);
		else if (token.size() == 1 && ((token.at(0).unicode() >= 'A' && token.at(0).unicode() <= 'Z') ||
									   (token.at(0).unicode() >= '0' && token.at(0).unicode() <= '9')))
			virtualKey = token.at(0).unicode();
		else
			return false;
	}

	// a global hotkey without any modifier would swallow the key for the whole system
	return (virtualKey != 0 && modifiers != 0);
}

void SystrayHandler::updateHotkeys()
{
	if (!_haveSystray)
		return;

	SystrayUnregisterHotkeys();

	if (_brightnessManager == nullptr || !_brightnessManager->areHotkeysEnabled())
		return;

	const struct {
		int				id;
		const char*		action;
	} hotkeyActions[] = {
		{ HOTKEY_BRIGHTNESS_UP,		"up" },
		{ HOTKEY_BRIGHTNESS_DOWN,	"down" },
		{ HOTKEY_NIGHT_MODE,		"nightMode" }
	};

	for (const auto& hotkeyAction : hotkeyActions)
	{
		const QString definition = _brightnessManager->getHotkey(QString::fromLatin1(hotkeyAction.action)).trimmed();

		if (definition.isEmpty())
			continue;

		unsigned int modifiers = 0, virtualKey = 0;

		if (!parseHotkey(definition, modifiers, virtualKey))
		{
			printf("Could not parse the '%s' hotkey definition: %s\n", hotkeyAction.action, definition.toStdString().c_str());
			continue;
		}

		if (!SystrayRegisterHotkey(hotkeyAction.id, modifiers, virtualKey))
		{
			printf("Could not register the '%s' hotkey (%s). It is probably already used by another application.\n",
				hotkeyAction.action, definition.toStdString().c_str());
		}
	}
}
#endif

void SystrayHandler::setColor(ColorRgb color)
{
	std::shared_ptr<HyperHdrManager> instanceManager = _instanceManager.lock();
	if (instanceManager)
		QUEUE_CALL_4(instanceManager.get(), setInstanceColor, int, _selectedInstance, int, 1, ColorRgb, color, int, 0);
}


void SystrayHandler::settings()
{
#ifndef _WIN32
	// Hide error messages when opening webbrowser

	int out_pipe[2];
	int saved_stdout;
	int saved_stderr;

	// saving stdout and stderr file descriptor
	saved_stdout = ::dup(STDOUT_FILENO);
	saved_stderr = ::dup(STDERR_FILENO);

	if (::pipe(out_pipe) == 0)
	{
		// redirecting stdout to pipe
		::dup2(out_pipe[1], STDOUT_FILENO);
		::close(out_pipe[1]);
		// redirecting stderr to stdout
		::dup2(STDOUT_FILENO, STDERR_FILENO);
	}
#endif

	QString link = QString("http://localhost:%1/").arg(_webPort);

#ifdef _WIN32
	const wchar_t* array = (const wchar_t*)link.utf16();
	ShellExecuteW(0, 0, array, 0, 0, SW_SHOW);
#endif

#ifdef __linux__
	QString command = QString("xdg-open %1").arg(link);
	QByteArray commandUtf8 = command.toUtf8();
	if (system((commandUtf8.constData())) == -1)
	{
		printf("xdg-open <http_link> failed. xdg-utils package is required.\n");
	}
#endif
	
#ifdef __APPLE__
	std::string slink = link.toStdString();
	CFURLRef url = CFURLCreateWithBytes(
		NULL,                       
		(UInt8*)slink.c_str(),
		slink.length(),
		kCFStringEncodingASCII,
		NULL
	);
	LSOpenCFURLRef(url, 0);
	CFRelease(url);
#endif

#ifndef _WIN32
	// restoring stdout
	::dup2(saved_stdout, STDOUT_FILENO);
	// restoring stderr
	::dup2(saved_stderr, STDERR_FILENO);
#endif
}

void SystrayHandler::setEffect(QString effect)
{
	std::shared_ptr<HyperHdrManager> instanceManager = _instanceManager.lock();
	if (instanceManager)
		QUEUE_CALL_3(instanceManager.get(), setInstanceEffect, int, _selectedInstance, QString, effect, int, 1);
}

void SystrayHandler::clearEfxColor()
{
	std::shared_ptr<HyperHdrManager> instanceManager = _instanceManager.lock();
	if (instanceManager)
		QUEUE_CALL_2(instanceManager.get(), clearInstancePriority, int, _selectedInstance, int, 1);
}

void SystrayHandler::signalInstanceStateChangedHandler(InstanceState state, quint8 instance, const QString& /*name*/)
{
	if (instance == _selectedInstance && state == InstanceState::STOP)
		_selectedInstance = -1;

	createSystray();
}

void SystrayHandler::signalSettingsChangedHandler(settings::type type, const QJsonDocument& data)
{
	if (type == settings::type::WEBSERVER)
	{
		_webPort = data.object()["port"].toInt();
	}
}

