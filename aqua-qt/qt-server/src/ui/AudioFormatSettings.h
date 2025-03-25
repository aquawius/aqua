//
// Created by QU on 25-3-26.
//

#ifndef AUDIOFORMATSETTINGS_H
#define AUDIOFORMATSETTINGS_H

#include "audio_format_common.hpp"
#include <QDialog>

QT_BEGIN_NAMESPACE

namespace Ui {
class AudioFormatSettings;
}

QT_END_NAMESPACE

class AudioFormatSettings : public QDialog {
    Q_OBJECT

public:
    explicit AudioFormatSettings(QWidget* parent = nullptr);
    ~AudioFormatSettings() override;

    // 设置初始的音频格式
    void setAudioFormat(const audio_common::AudioFormat& format);

    // 获取用户选择的音频格式
    audio_common::AudioFormat getAudioFormat() const;

private Q_SLOTS:
    void onOkButtonClicked();
    void onCancelButtonClicked();

Q_SIGNALS:
    void formatAccepted(const audio_common::AudioFormat& format);
    void formatRejected();

private:
    Ui::AudioFormatSettings* ui;

    // 编码字符串转换为AudioEncoding枚举
    audio_common::AudioEncoding getEncodingFromComboBox() const;
};

#endif //AUDIOFORMATSETTINGS_H