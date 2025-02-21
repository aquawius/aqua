//
// Created by aquawius on 25-2-20.
//

#include "AudioMeterWidget.h"
#include <QPainter>
#include <QtMath>
#include <QDateTime>
#include <cmath>

AudioMeterWidget::AudioMeterWidget(QWidget* parent)
    : QWidget(parent)
{
    // 初始化渐变颜色
    m_gradient.setColorAt(0.0, QColor(55, 210, 71));
    m_gradient.setColorAt(0.55, QColor(55, 210, 71));
    m_gradient.setColorAt(0.75, QColor(229, 175, 36));
    m_gradient.setColorAt(0.95, QColor(227, 59, 87));

    // 初始化峰值保持线的半透明渐变
    m_hold_gradient.setColorAt(0.0, QColor(255, 255, 255, 150));
    m_hold_gradient.setColorAt(1.0, QColor(200, 200, 200, 200));

    // 设置高精度定时器（约100FPS）
    m_update_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_update_timer, &QTimer::timeout, this, &AudioMeterWidget::updateMeter);
    m_update_timer.start(16); // ~60FPS
    m_last_update_time = QDateTime::currentMSecsSinceEpoch();
}

void AudioMeterWidget::setPeakValue(float peak)
{
    // 限制输入值在[0,1]范围并更新当前峰值
    m_current_peak = qBound(0.0f, peak, 1.0f);

    // 如果当前峰值超过保持峰值，更新保持峰值并重置保持时间
    if (m_current_peak > m_peak_hold)
    {
        m_peak_hold = m_current_peak;
        m_peak_hold_expiry = QDateTime::currentMSecsSinceEpoch() + 500; // 保持500ms后开始衰减
    }
}

void AudioMeterWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing); // 启用抗锯齿

    const QRect rect = this->rect();
    const int height = rect.height();

    // 绘制黑色背景
    painter.fillRect(rect, Qt::black);

    // 设置垂直渐变方向（从下到上）
    m_gradient.setStart(0, height);
    m_gradient.setFinalStop(0, 0);

    // 计算当前显示高度和保持高度
    int meter_height = static_cast<int>(height * m_display_peak);
    int hold_height = static_cast<int>(height * m_peak_hold);

    // 绘制主音量条
    painter.fillRect(rect.left(), height - meter_height,
                     rect.width(), meter_height, m_gradient);

    // 当保持峰值超过1%时绘制保持线
    if (m_peak_hold > 0.01f)
    {
        QPen hold_pen(QBrush(m_hold_gradient), 2);
        painter.setPen(hold_pen);
        painter.drawLine(rect.left(), height - hold_height, rect.right(), height - hold_height);
    }
}

void AudioMeterWidget::updateMeter()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qreal elapsed_sec = (now - m_last_update_time) / 1000.0; // 计算经过时间（秒）
    m_last_update_time = now;

    if (m_current_peak > m_display_peak)
    {
        m_display_peak = m_current_peak;
    }
    else
    {
        m_display_peak *= std::exp(-elapsed_sec / m_release_time);
    }
    m_display_peak = qBound(0.0f, m_display_peak, 1.0f);

    if (now >= m_peak_hold_expiry)
    {
        m_peak_hold *= std::exp(-elapsed_sec / m_hold_decay_time);
        m_peak_hold = qMax(0.0f, m_peak_hold);
    }

    update(); // 请求重绘界面
}
