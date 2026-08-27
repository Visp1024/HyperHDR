/* BrightnessManager.h
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

#pragma once

#ifndef PCH_ENABLED
	#include <QObject>
	#include <QJsonDocument>
	#include <QString>
	#include <QTimer>

	#include <memory>
#endif

#include <QDateTime>

#include <utils/Logger.h>
#include <utils/settings.h>

class HyperHdrManager;

namespace hyperhdr { enum class InstanceState; }

/// Keeps the global (all instances) user brightness and the night mode.
/// The brightness is a runtime multiplier applied on top of the color calibration,
/// so it never overwrites the user's channel adjustment.
class BrightnessManager : public QObject
{
	Q_OBJECT

public:
	BrightnessManager(std::shared_ptr<HyperHdrManager> instanceManager, const QJsonDocument& config, QObject* parent = nullptr);

	/// brightness of the current mode (night brightness when the night mode is active)
	int getBrightness() const;
	bool isNightMode() const;
	bool areHotkeysEnabled() const;
	QString getHotkey(const QString& action) const;

public slots:
	void setBrightness(int percent);
	void changeBrightness(int delta);
	void increaseBrightness();
	void decreaseBrightness();
	void setNightMode(bool enable);
	void toggleNightMode();
	void handleSettingsChangedHandler(settings::type type, const QJsonDocument& config);
	void handleInstanceStateChangedHandler(hyperhdr::InstanceState state, quint8 instance, const QString& name);

signals:
	/// the tray (or any other UI) should refresh itself
	void SignalBrightnessChanged(int brightness, bool nightMode);
	/// the hotkey configuration has been changed
	void SignalHotkeysChanged();

private slots:
	void checkSchedule();

private:
	void applySettings(const QJsonObject& config, bool firstRun);
	void applyToInstances();
	void saveBrightness(const QString& key, int percent);
	static bool parseTime(const QString& text, QTime& result);
	bool isScheduledNightTime(const QTime& now) const;

	LoggerName						_log;
	std::weak_ptr<HyperHdrManager>	_instanceManager;

	int			_dayBrightness;
	int			_nightBrightness;
	int			_step;
	bool		_nightMode;
	bool		_scheduleEnabled;
	QTime		_scheduleStart;
	QTime		_scheduleEnd;
	int			_lastScheduleState;
	bool		_hotkeysEnabled;
	QString		_hotkeyBrightnessUp;
	QString		_hotkeyBrightnessDown;
	QString		_hotkeyNightMode;
	QTimer		_scheduleTimer;
};
