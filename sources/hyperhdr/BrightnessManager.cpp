/* BrightnessManager.cpp
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
	#include <QJsonObject>

	#include <algorithm>
#endif

#include <base/HyperHdrInstance.h>
#include <base/HyperHdrManager.h>

#include "BrightnessManager.h"

namespace
{
	const int	BRIGHTNESS_MIN = 5;
	const int	BRIGHTNESS_MAX = 100;
	const int	SCHEDULE_CHECK_INTERVAL_MS = 20000;
}

BrightnessManager::BrightnessManager(std::shared_ptr<HyperHdrManager> instanceManager, const QJsonDocument& config, QObject* parent)
	: QObject(parent),
	_log("BRIGHTNESS"),
	_instanceManager(instanceManager),
	_dayBrightness(BRIGHTNESS_MAX),
	_nightBrightness(30),
	_step(10),
	_nightMode(false),
	_scheduleEnabled(false),
	_scheduleStart(22, 0),
	_scheduleEnd(7, 0),
	_lastScheduleState(-1),
	_hotkeysEnabled(true),
	_hotkeyBrightnessUp("Ctrl+Alt+Up"),
	_hotkeyBrightnessDown("Ctrl+Alt+Down"),
	_hotkeyNightMode("Ctrl+Alt+N")
{
	if (instanceManager != nullptr)
	{
		connect(instanceManager.get(), &HyperHdrManager::SignalSettingsChanged, this, &BrightnessManager::handleSettingsChangedHandler);
		connect(instanceManager.get(), &HyperHdrManager::SignalInstanceStateChanged, this, &BrightnessManager::handleInstanceStateChangedHandler);
	}

	_scheduleTimer.setInterval(SCHEDULE_CHECK_INTERVAL_MS);
	connect(&_scheduleTimer, &QTimer::timeout, this, &BrightnessManager::checkSchedule);

	applySettings(config.object(), true);
}

int BrightnessManager::getBrightness() const
{
	return (_nightMode) ? _nightBrightness : _dayBrightness;
}

bool BrightnessManager::isNightMode() const
{
	return _nightMode;
}

bool BrightnessManager::areHotkeysEnabled() const
{
	return _hotkeysEnabled;
}

QString BrightnessManager::getHotkey(const QString& action) const
{
	if (action == "up")
		return _hotkeyBrightnessUp;
	else if (action == "down")
		return _hotkeyBrightnessDown;
	else if (action == "nightMode")
		return _hotkeyNightMode;

	return QString();
}

bool BrightnessManager::parseTime(const QString& text, QTime& result)
{
	QTime parsed = QTime::fromString(text.trimmed(), QString("HH:mm"));

	if (!parsed.isValid())
		parsed = QTime::fromString(text.trimmed(), QString("H:mm"));

	if (!parsed.isValid())
		return false;

	result = parsed;
	return true;
}

bool BrightnessManager::isScheduledNightTime(const QTime& now) const
{
	if (_scheduleStart == _scheduleEnd)
		return false;

	if (_scheduleStart < _scheduleEnd)
		return (now >= _scheduleStart && now < _scheduleEnd);

	// the night usually crosses the midnight
	return (now >= _scheduleStart || now < _scheduleEnd);
}

void BrightnessManager::applySettings(const QJsonObject& config, bool firstRun)
{
	const int newDay = std::clamp(config["brightness"].toInt(_dayBrightness), BRIGHTNESS_MIN, BRIGHTNESS_MAX);
	const int newNight = std::clamp(config["nightModeBrightness"].toInt(_nightBrightness), BRIGHTNESS_MIN, BRIGHTNESS_MAX);
	const int newStep = std::clamp(config["step"].toInt(_step), 1, 50);
	const bool newSchedule = config["nightModeSchedule"].toBool(_scheduleEnabled);

	QTime start = _scheduleStart, end = _scheduleEnd;
	if (!parseTime(config["nightModeStart"].toString("22:00"), start))
		Warning(_log, "Invalid night mode start time, using {:s}", (start.toString(QString("HH:mm"))));
	if (!parseTime(config["nightModeEnd"].toString("07:00"), end))
		Warning(_log, "Invalid night mode end time, using {:s}", (end.toString(QString("HH:mm"))));

	const bool hotkeysEnabled = config["hotkeysEnable"].toBool(_hotkeysEnabled);
	const QString hotkeyUp = config["hotkeyBrightnessUp"].toString(_hotkeyBrightnessUp);
	const QString hotkeyDown = config["hotkeyBrightnessDown"].toString(_hotkeyBrightnessDown);
	const QString hotkeyNight = config["hotkeyNightMode"].toString(_hotkeyNightMode);

	const bool hotkeysChanged = (hotkeysEnabled != _hotkeysEnabled) || (hotkeyUp != _hotkeyBrightnessUp) ||
								(hotkeyDown != _hotkeyBrightnessDown) || (hotkeyNight != _hotkeyNightMode);
	const bool scheduleChanged = (newSchedule != _scheduleEnabled) || (start != _scheduleStart) || (end != _scheduleEnd);
	const bool brightnessChanged = (newDay != _dayBrightness) || (newNight != _nightBrightness);

	_dayBrightness = newDay;
	_nightBrightness = newNight;
	_step = newStep;
	_scheduleEnabled = newSchedule;
	_scheduleStart = start;
	_scheduleEnd = end;
	_hotkeysEnabled = hotkeysEnabled;
	_hotkeyBrightnessUp = hotkeyUp;
	_hotkeyBrightnessDown = hotkeyDown;
	_hotkeyNightMode = hotkeyNight;

	if (firstRun)
	{
		Info(_log, "Brightness: {:d}%, night mode brightness: {:d}%, schedule: {:s} ({:s} - {:s})",
			_dayBrightness, _nightBrightness, ((_scheduleEnabled) ? "enabled" : "disabled"),
			(_scheduleStart.toString(QString("HH:mm"))), (_scheduleEnd.toString(QString("HH:mm"))));
	}

	if (scheduleChanged || firstRun)
	{
		_lastScheduleState = -1;
		_scheduleTimer.stop();

		if (_scheduleEnabled)
		{
			checkSchedule();
			_scheduleTimer.start();
		}
	}

	if (brightnessChanged || firstRun)
	{
		applyToInstances();
		emit SignalBrightnessChanged(getBrightness(), _nightMode);
	}

	if (hotkeysChanged)
		emit SignalHotkeysChanged();
}

void BrightnessManager::checkSchedule()
{
	if (!_scheduleEnabled)
		return;

	const bool isNight = isScheduledNightTime(QTime::currentTime());
	const int state = (isNight) ? 1 : 0;

	// the schedule acts on its edges only, so a manual override survives until the next one
	if (state == _lastScheduleState)
		return;

	_lastScheduleState = state;

	if (isNight != _nightMode)
	{
		Info(_log, "The night mode is turned {:s} by the schedule", ((isNight) ? "on" : "off"));
		setNightMode(isNight);
	}
}

void BrightnessManager::applyToInstances()
{
	std::shared_ptr<HyperHdrManager> instanceManager = _instanceManager.lock();

	if (instanceManager == nullptr)
		return;

	const float scale = getBrightness() / 100.0f;
	QUEUE_CALL_1(instanceManager.get(), setInstancesBrightnessScale, float, scale);
}

void BrightnessManager::saveBrightness(const QString& key, int percent)
{
	std::shared_ptr<HyperHdrManager> instanceManager = _instanceManager.lock();

	if (instanceManager == nullptr || !instanceManager->IsInstanceRunning(0))
		return;

	auto instance = instanceManager->getHyperHdrInstance(0);

	if (instance == nullptr)
		return;

	QJsonDocument current;
	SAFE_CALL_1_RET(instance.get(), getSetting, QJsonDocument, current, settings::type, settings::type::BRIGHTNESS);

	QJsonObject config = current.object();
	config[key] = percent;

	QString serialized = QString(QJsonDocument(config).toJson(QJsonDocument::Compact));
	QUEUE_CALL_2(instance.get(), setSetting, settings::type, settings::type::BRIGHTNESS, QString, serialized);
}

void BrightnessManager::setBrightness(int percent)
{
	percent = std::clamp(percent, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

	int& target = (_nightMode) ? _nightBrightness : _dayBrightness;

	if (target == percent)
		return;

	target = percent;

	applyToInstances();
	emit SignalBrightnessChanged(getBrightness(), _nightMode);

	saveBrightness((_nightMode) ? QString("nightModeBrightness") : QString("brightness"), percent);
}

void BrightnessManager::changeBrightness(int delta)
{
	setBrightness(getBrightness() + delta);
}

void BrightnessManager::increaseBrightness()
{
	changeBrightness(_step);
}

void BrightnessManager::decreaseBrightness()
{
	changeBrightness(-_step);
}

void BrightnessManager::setNightMode(bool enable)
{
	if (_nightMode == enable)
		return;

	_nightMode = enable;

	Info(_log, "The night mode is {:s} (brightness: {:d}%)", ((_nightMode) ? "enabled" : "disabled"), getBrightness());

	applyToInstances();
	emit SignalBrightnessChanged(getBrightness(), _nightMode);
}

void BrightnessManager::toggleNightMode()
{
	setNightMode(!_nightMode);
}

void BrightnessManager::handleSettingsChangedHandler(settings::type type, const QJsonDocument& config)
{
	if (type == settings::type::BRIGHTNESS)
		applySettings(config.object(), false);
}

void BrightnessManager::handleInstanceStateChangedHandler(hyperhdr::InstanceState state, quint8 /*instance*/, const QString& /*name*/)
{
	// a fresh instance starts with the neutral brightness, so it has to be updated
	if (state == hyperhdr::InstanceState::START)
		applyToInstances();
}
