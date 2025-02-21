//
// Created by aquawius on 25-2-20.
//

#ifndef AUDIOMETERWIDGET_H
#define AUDIOMETERWIDGET_H

#include <QWidget>
#include <QTimer>
#include <QLinearGradient>
#include <QDateTime>

class AudioMeterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AudioMeterWidget(QWidget* parent = nullptr);
    void setPeakValue(float peak);

protected:
    void paintEvent(QPaintEvent* event) override;

private Q_SLOTS:
    void updateMeter();

private:
    float m_current_peak = 0.0f;
    float m_display_peak = 0.0f;
    float m_peak_hold = 0.0f;
    qint64 m_last_update_time = 0;
    qint64 m_peak_hold_expiry = 0;

    QTimer m_update_timer;
    QLinearGradient m_gradient;
    QLinearGradient m_hold_gradient;

    const float m_release_time = 0.5f;    // 增大此值使主峰值条下降更慢
    const float m_hold_decay_time = 2.0f; // 增大此值使峰值保持线下降更慢
};

#endif // AUDIOMETERWIDGET_H
