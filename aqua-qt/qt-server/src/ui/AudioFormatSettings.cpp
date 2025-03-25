//
// Created by QU on 25-3-26.
//

// You may need to build the project (run Qt uic code generator) to get "ui_AudioFormatSettings.h" resolved

#include "AudioFormatSettings.h"
#include "ui_AudioFormatSettings.h"
#include <QMessageBox>

AudioFormatSettings::AudioFormatSettings(QWidget* parent) :
    QDialog(parent), ui(new Ui::AudioFormatSettings)
{
    ui->setupUi(this);

    // 连接信号和槽
    connect(ui->pushButtonOK, &QPushButton::clicked, this, &AudioFormatSettings::onOkButtonClicked);
    connect(ui->pushButtonCancel, &QPushButton::clicked, this, &AudioFormatSettings::onCancelButtonClicked);
}

AudioFormatSettings::~AudioFormatSettings()
{
    delete ui;
}

void AudioFormatSettings::setAudioFormat(const audio_common::AudioFormat& format)
{
    // 设置编码
    int encodingIndex = 0; // INVALID

    switch (format.encoding) {
    case audio_common::AudioEncoding::PCM_S16LE:
        encodingIndex = 1;
        break;
    case audio_common::AudioEncoding::PCM_S32LE:
        encodingIndex = 2;
        break;
    case audio_common::AudioEncoding::PCM_F32LE:
        encodingIndex = 3;
        break;
    case audio_common::AudioEncoding::PCM_S24LE:
        encodingIndex = 4;
        break;
    case audio_common::AudioEncoding::PCM_U8:
        encodingIndex = 5;
        break;
    default:
        encodingIndex = 0; // INVALID
        break;
    }

    ui->comboBoxEncoding->setCurrentIndex(encodingIndex);

    // 设置通道数和采样率
    ui->spinBoxChannels->setValue(format.channels);
    ui->spinBoxSampleRate->setValue(format.sample_rate);
}

audio_common::AudioFormat AudioFormatSettings::getAudioFormat() const
{
    // 创建AudioFormat对象
    return audio_common::AudioFormat(
        getEncodingFromComboBox(),
        ui->spinBoxChannels->value(),
        ui->spinBoxSampleRate->value()
        );
}

audio_common::AudioEncoding AudioFormatSettings::getEncodingFromComboBox() const
{
    // 将comboBox的选择转换为AudioEncoding枚举
    switch (ui->comboBoxEncoding->currentIndex()) {
    case 1:
        return audio_common::AudioEncoding::PCM_S16LE;
    case 2:
        return audio_common::AudioEncoding::PCM_S32LE;
    case 3:
        return audio_common::AudioEncoding::PCM_F32LE;
    case 4:
        return audio_common::AudioEncoding::PCM_S24LE;
    case 5:
        return audio_common::AudioEncoding::PCM_U8;
    case 0:
    default:
        return audio_common::AudioEncoding::INVALID;
    }
}

void AudioFormatSettings::onOkButtonClicked()
{
    // 获取设置的音频格式
    auto format = getAudioFormat();

    // 验证格式是否有效
    if (format.encoding == audio_common::AudioEncoding::INVALID) {
        QMessageBox::warning(this, tr("Invalid Format"),
            tr("Please select a valid audio encoding."));
        return;
    }

    // 验证通道数和采样率
    if (!audio_common::AudioFormat::is_valid(format)) {
        QMessageBox::warning(this, tr("Invalid Format"),
            tr("The audio format settings are not valid."));
        return;
    }

    // 发出信号，通知格式已被接受
    emit formatAccepted(format);
    this->close();
}

void AudioFormatSettings::onCancelButtonClicked()
{
    // 发出取消信号
    emit formatRejected();
    this->close();
}