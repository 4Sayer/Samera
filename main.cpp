// SPDX-License-Identifier: MIT
#include <QtWidgets>
#include "i18n.h"
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QMediaDevices>
#include <QAudioDevice>
#include <cmath>
#include <QVideoSink>
#include <QVideoFrame>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <memory>

class TransparentMenu : public QMenu {
public:
    explicit TransparentMenu(QWidget *parent=nullptr):QMenu(parent){
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet("QMenu{background:rgba(0,0,0,204);color:#eee;border:1px solid rgba(255,255,255,45);border-radius:7px;padding:5px;} QMenu::item{padding:7px 22px;background:transparent;} QMenu::item:selected{background:rgba(110,130,160,160);border-radius:3px;}");
    }
};
static QMenu *submenu(QMenu *parent,const QString &title){auto *menu=new TransparentMenu(parent);menu->setTitle(title);parent->addMenu(menu);return menu;}

struct CameraChoice {
    QString id, label;
    std::shared_ptr<GstDevice> device;
};
static QList<CameraChoice> cameras() {
    QList<CameraChoice> result;
    GstDeviceMonitor *monitor=gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor,"Video/Source",nullptr);
    gst_device_monitor_start(monitor);
    GList *devices=gst_device_monitor_get_devices(monitor);
    for(GList *it=devices;it;it=it->next){
        auto *device=GST_DEVICE(it->data);GstStructure *props=gst_device_get_properties(device);
        const char *path=props?gst_structure_get_string(props,"api.v4l2.path"):nullptr;
        if(!path&&props)path=gst_structure_get_string(props,"device.path");
        QString id=path?QString::fromUtf8(path):QString();
        if(props)gst_structure_free(props);
        if(id.isEmpty())continue;
        bool duplicate=false;for(const auto &entry:result)if(entry.id==id)duplicate=true;
        if(duplicate)continue;
        gchar *name=gst_device_get_display_name(device);QString label=QString::fromUtf8(name);g_free(name);
        if(id=="/dev/video0")label+=i18n(" · Color");
        else if(id=="/dev/video2")label+=i18n(" · Infrarroja");
        result.append({id,label,std::shared_ptr<GstDevice>(GST_DEVICE(gst_object_ref(device)),[](GstDevice *d){gst_object_unref(d);})});
    }
    g_list_free_full(devices,gst_object_unref);gst_device_monitor_stop(monitor);gst_object_unref(monitor);
    return result;
}
static double frameRate(const GValue *value) {
    if(!value)return 30;
    if(GST_VALUE_HOLDS_FRACTION(value))return double(gst_value_get_fraction_numerator(value))/gst_value_get_fraction_denominator(value);
    if(GST_VALUE_HOLDS_FRACTION_RANGE(value))return frameRate(gst_value_get_fraction_range_max(value));
    if(GST_VALUE_HOLDS_LIST(value)){double rate=0;for(guint i=0;i<gst_value_list_get_size(value);++i)rate=qMax(rate,frameRate(gst_value_list_get_value(value,i)));return rate;}
    return 30;
}
static QString captureCaps(GstDevice *device,int maxHeight,int requestedFps,int &width,int &height,int &fps,bool &jpeg) {
    GstCaps *caps=gst_device_get_caps(device);double best=-1;QString selected;
    for(guint i=0;caps&&i<gst_caps_get_size(caps);++i){
        const GstStructure *structure=gst_caps_get_structure(caps,i);
        QString name=QString::fromUtf8(gst_structure_get_name(structure));
        if(name!="image/jpeg"&&name!="video/x-raw")continue;
        const GstCapsFeatures *features=gst_caps_get_features(caps,i);
        if(gst_caps_features_contains(features,"memory:DMABuf"))continue;
        int w=0,h=0;if(!gst_structure_get_int(structure,"width",&w)||!gst_structure_get_int(structure,"height",&h))continue;
        if(w<=0||h<=0||w>7680||h>4320)continue;
        int rate=qMax(1,int(qMin(double(requestedFps),frameRate(gst_structure_get_value(structure,"framerate")))));
        // Prefer full frame rate and requested size, then the best available resolution.
        double score=(rate>=qMin(requestedFps,24)?1e12:0)+(h<=maxHeight?1e10:0)+double(w)*h*100+rate;
        if(name=="image/jpeg")score+=10;
        if(score<=best)continue;
        GstCaps *one=gst_caps_copy_nth(caps,i);
        GstStructure *chosen=gst_caps_get_structure(one,0);
        gst_structure_set(chosen,"framerate",GST_TYPE_FRACTION,rate,1,nullptr);
        gchar *text=gst_caps_to_string(one);selected=QString::fromUtf8(text);g_free(text);gst_caps_unref(one);
        best=score;width=w;height=h;fps=rate;jpeg=name=="image/jpeg";
    }
    if(caps)gst_caps_unref(caps);
    return selected;
}
struct MicrophoneChoice { QString id,label; };
static QList<MicrophoneChoice> microphones(){
    QList<MicrophoneChoice> result{{QString(),i18n("Predeterminado del sistema")}};
    QProcess process;process.start("pactl",{"--format=json","list","sources"});
    if(!process.waitForFinished(2000)){process.kill();process.waitForFinished();return result;}
    const QJsonArray sources=QJsonDocument::fromJson(process.readAllStandardOutput()).array();
    for(const QJsonValue &value:sources){const QJsonObject source=value.toObject();
        if(source.value("monitor_of_sink").toString()!=""&&source.value("monitor_of_sink").toString()!="n/a")continue;
        const QString id=source.value("name").toString();if(id.endsWith(".monitor"))continue;
        if(!id.isEmpty())result.append({id,source.value("description").toString()});
    }
    return result;
}

static const QString SaveDir = qEnvironmentVariable("SAMERA_TEST_DIR",
    QDir::homePath() + "/Pictures/Camera");
static QString filename(const QString &extension) {
    return SaveDir + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") + "." + extension;
}
static QString gstError(GstMessage *message) {
    GError *err = nullptr; gchar *debug = nullptr;
    gst_message_parse_error(message, &err, &debug);
    QString text = err ? QString::fromUtf8(err->message) : i18n("Error de cámara");
    if (err) g_error_free(err);
    g_free(debug); return text;
}

static bool writeTestTone(const QString &path){
    QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
    const int samples=57600;QDataStream out(&file);out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF",4);out<<quint32(36+samples*2);out.writeRawData("WAVEfmt ",8);
    out<<quint32(16)<<quint16(1)<<quint16(1)<<quint32(48000)<<quint32(96000)<<quint16(2)<<quint16(16);
    out.writeRawData("data",4);out<<quint32(samples*2);
    for(int i=0;i<samples;++i){
        double fade=qMin(1.0,qMin(i/600.0,(samples-i-1)/600.0));
        out<<qint16(1500*fade*std::sin(2*3.141592653589793*480*i/48000));
    }
    return out.status()==QDataStream::Ok;
}

class AudioProbe {
public:
    GstElement *pipeline=nullptr;
    std::atomic<double> db{-100},maximum{-100};
    std::atomic<unsigned long> buffers{0};
    QString recordingPath;
    ~AudioProbe(){stop();}
    static GstFlowReturn sample(GstAppSink *sink,gpointer context){
        auto *self=static_cast<AudioProbe *>(context);GstSample *sample=gst_app_sink_pull_sample(sink);
        if(!sample)return GST_FLOW_EOS;
        GstMapInfo mapped;
        if(gst_buffer_map(gst_sample_get_buffer(sample),&mapped,GST_MAP_READ)){
            const float *values=reinterpret_cast<const float *>(mapped.data);double peak=0;
            for(gsize i=0;i<mapped.size/sizeof(float);++i)peak=qMax(peak,double(std::abs(values[i])));
            double level=peak>1e-9?20*std::log10(peak):-100;
            self->db=level;self->maximum=qMax(self->maximum.load(),level);self->buffers++;
            gst_buffer_unmap(gst_sample_get_buffer(sample),&mapped);
        }
        gst_sample_unref(sample);return GST_FLOW_OK;
    }
    bool start(const QString &device,double gain,const QString &path=QString()){
        stop();db=-100;maximum=-100;buffers=0;recordingPath=path;
        QString desc="pulsesrc name=source do-timestamp=true ! volume name=gain ! audioconvert ! audioresample ! audio/x-raw,format=F32LE,rate=48000,channels=1 ! tee name=t t. ! queue ! appsink name=meter max-buffers=1 drop=true sync=false";
        if(!path.isEmpty())desc+=" t. ! queue ! audioconvert ! audio/x-raw,format=S16LE ! wavenc ! filesink name=output";
        GError *error=nullptr;pipeline=gst_parse_launch(desc.toUtf8().constData(),&error);
        if(error){g_error_free(error);stop();return false;}
        if(!pipeline)return false;
        GstElement *source=gst_bin_get_by_name(GST_BIN(pipeline),"source");
        if(!device.isEmpty())g_object_set(source,"device",device.toUtf8().constData(),nullptr);
        gst_object_unref(source);
        GstElement *volume=gst_bin_get_by_name(GST_BIN(pipeline),"gain");g_object_set(volume,"volume",gain,nullptr);gst_object_unref(volume);
        if(!path.isEmpty()){
            GstElement *output=gst_bin_get_by_name(GST_BIN(pipeline),"output");g_object_set(output,"location",path.toUtf8().constData(),nullptr);gst_object_unref(output);
        }
        GstElement *meter=gst_bin_get_by_name(GST_BIN(pipeline),"meter");GstAppSinkCallbacks callbacks{};callbacks.new_sample=sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(meter),&callbacks,this,nullptr);gst_object_unref(meter);
        return gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE;
    }
    QString error(){
        if(!pipeline)return i18n("No se pudo iniciar el micrófono.");
        GstBus *bus=gst_element_get_bus(pipeline);GstMessage *message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);QString result;
        if(message){result=gstError(message);gst_message_unref(message);}gst_object_unref(bus);return result;
    }
    void stop(){
        if(!pipeline)return;
        if(!recordingPath.isEmpty()){
            gst_element_send_event(pipeline,gst_event_new_eos());GstBus *bus=gst_element_get_bus(pipeline);
            GstMessage *message=gst_bus_timed_pop_filtered(bus,2*GST_SECOND,static_cast<GstMessageType>(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
            if(message)gst_message_unref(message);
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);pipeline=nullptr;
    }
};

class Camera {
public:
    GstElement *pipeline = nullptr;
    std::mutex frameMutex, recordingMutex;
    QImage latest;
    std::atomic<unsigned long> frames{0};
    std::atomic<bool> mirror{true};
    std::atomic<int> rotation{0};
    std::atomic<int> effect{0};
    GstElement *recording = nullptr, *recordSource = nullptr;
    QString pendingPath;
    QString recordedError;
    QImage videoPoster;
    bool test = false;
    QString selectedId="/dev/video0",microphoneId;
    int maxHeight=1080,requestedFps=30,width=1920,height=1080,actualFps=30;
    double microphoneGain=1.0;
    ~Camera() { stop(); }
    static GstFlowReturn sample(GstAppSink *sink, gpointer data) {
        auto *self = static_cast<Camera *>(data);
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        GstVideoInfo info;
        gst_video_info_init(&info);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstVideoFrame mapped;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
            gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) {
            QImage image(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0)),
                         GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                         GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0), QImage::Format_RGB888);
            image = image.copy();
            gst_video_frame_unmap(&mapped);
            if (self->mirror.load()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                image = image.flipped(Qt::Horizontal);
#else
                image = image.mirrored(true, false);
#endif
            }
            const int angle = self->rotation.load();
            if (angle) image = image.transformed(QTransform().rotate(angle));
            const int effect = self->effect.load();
            if (effect == 1) image = image.convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_RGB888);
            else if (effect == 2) {
                for (int y=0; y<image.height(); ++y) {
                    uchar *line = image.scanLine(y);
                    for (int x=0; x<image.width(); ++x) {
                        uchar *pixel = line + x*3;
                        int r=pixel[0],g=pixel[1],b=pixel[2];
                        pixel[0]=qMin(255,(393*r+769*g+189*b)/1000);
                        pixel[1]=qMin(255,(349*r+686*g+168*b)/1000);
                        pixel[2]=qMin(255,(272*r+534*g+131*b)/1000);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(self->frameMutex);
                self->latest = image;
            }
            self->frames++;
            {
                std::lock_guard<std::mutex> lock(self->recordingMutex);
                if (self->recordSource) {
                    GstBuffer *out = gst_buffer_new_allocate(nullptr, image.sizeInBytes(), nullptr);
                    gst_buffer_fill(out, 0, image.constBits(), image.sizeInBytes());
                    GstClock *clock = gst_element_get_clock(self->recording);
                    if (clock) {
                        GST_BUFFER_PTS(out) = gst_clock_get_time(clock) - gst_element_get_base_time(self->recording);
                        gst_object_unref(clock);
                    } else GST_BUFFER_PTS(out) = 0;
                    GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
                    GST_BUFFER_DURATION(out) = GST_SECOND / self->actualFps;
                    gst_app_src_push_buffer(GST_APP_SRC(self->recordSource), out);
                }
            }
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    QImage frame() { std::lock_guard<std::mutex> lock(frameMutex); return latest; }
    bool start(bool synthetic, QString &error) {
        test = synthetic;
        GstElement *source = nullptr;
        bool jpeg=!synthetic;QString chosenCaps;
        if (synthetic) {
            source = gst_element_factory_make("videotestsrc", "camera-source");
            if (source) g_object_set(source, "is-live", TRUE, "pattern", 0, nullptr);
        } else {
            const auto choices=cameras();
            for(const auto &choice:choices)if(choice.id==selectedId){
                chosenCaps=captureCaps(choice.device.get(),maxHeight,requestedFps,width,height,actualFps,jpeg);
                if(!chosenCaps.isEmpty())source=gst_device_create_element(choice.device.get(),"camera-source");
                break;
            }
            if(chosenCaps.isEmpty()&&!choices.isEmpty()){
                error=i18n("La cámara seleccionada no está disponible. Elige otra en Opciones → Cámara.");return false;
            }
            if (!source) {
                source = gst_element_factory_make("v4l2src", "camera-source");
                if (source) g_object_set(source, "device", selectedId.toUtf8().constData(), nullptr);
            }
        }
        if (!source) { error = i18n("No se encuentra la cámara."); return false; }
        GError *parseError = nullptr;
        QString description="capsfilter name=input-caps ! ";
        if(jpeg)description+="jpegdec ! ";
        description+="videoconvert ! video/x-raw,format=RGB ! appsink name=frames max-buffers=1 drop=true sync=false";
        pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
        if (parseError || !pipeline) {
            error = parseError ? QString::fromUtf8(parseError->message) : i18n("No se pudo iniciar la cámara.");
            if (parseError) g_error_free(parseError);
            gst_object_unref(source); stop(); return false;
        }
        gst_bin_add(GST_BIN(pipeline), source);
        GstElement *filter = gst_bin_get_by_name(GST_BIN(pipeline), "input-caps");
        if(synthetic)chosenCaps="video/x-raw,width=1920,height=1080,framerate=30/1";
        if(chosenCaps.isEmpty())chosenCaps="image/jpeg,width=1920,height=1080,framerate=30/1";
        GstCaps *inputCaps=gst_caps_from_string(chosenCaps.toUtf8().constData());
        g_object_set(filter,"caps",inputCaps,nullptr);gst_caps_unref(inputCaps);
        bool linked = gst_element_link(source, filter); gst_object_unref(filter);
        if (!linked) { error = i18n("La cámara no admite el modo Full HD MJPEG."); stop(); return false; }
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
        GstAppSinkCallbacks callbacks{}; callbacks.new_sample = sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
        gst_object_unref(sink);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            error = i18n("No se pudo abrir la cámara. Cierra otras apps que la estén usando.");
            stop(); return false;
        }
        return true;
    }
    QString pollError() {
        QString result;
        if (pipeline) {
            GstBus *bus = gst_element_get_bus(pipeline);
            GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (message) { result = gstError(message); gst_message_unref(message); }
            gst_object_unref(bus);
        }
        std::lock_guard<std::mutex> lock(recordingMutex);
        if (recording) {
            GstBus *bus = gst_element_get_bus(recording);
            GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (message) { recordedError = gstError(message); result = recordedError; gst_message_unref(message); }
            gst_object_unref(bus);
        }
        return result;
    }
    bool beginVideo(const QString &path, bool audio, QString &error) {
        const QImage image = frame();
        if (image.isNull()) { error = i18n("Espera a que aparezca la cámara."); return false; }
        std::lock_guard<std::mutex> lock(recordingMutex);
        if (recording) return false;
        QString desc = "appsrc name=record-source is-live=true format=time block=false max-buffers=3 leaky-type=downstream ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! video/x-raw,format=I420 ! x264enc tune=zerolatency speed-preset=ultrafast pass=qual quantizer=18 key-int-max=30 threads=2 ! h264parse ! queue ! mux. mp4mux name=mux faststart=true ! filesink name=output";
        if (audio) desc += test
            ? " audiotestsrc is-live=true volume=0.03 ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=1 ! avenc_aac bitrate=192000 ! aacparse ! queue ! mux."
            : " pulsesrc name=microphone do-timestamp=true ! volume name=microphone-gain ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=1 ! avenc_aac bitrate=192000 ! aacparse ! queue ! mux.";
        GError *parseError = nullptr;
        recording = gst_parse_launch(desc.toUtf8().constData(), &parseError);
        if (parseError || !recording) {
            error = parseError ? QString::fromUtf8(parseError->message) : i18n("No se pudo iniciar el vídeo.");
            if (parseError) g_error_free(parseError);
            if (recording) gst_object_unref(recording);
            recording = nullptr; return false;
        }
        recordedError.clear(); pendingPath = path; videoPoster = image;
        if(audio&&!test){
            GstElement *microphone=gst_bin_get_by_name(GST_BIN(recording),"microphone");
            if(!microphoneId.isEmpty())g_object_set(microphone,"device",microphoneId.toUtf8().constData(),nullptr);
            gst_object_unref(microphone);
            GstElement *gain=gst_bin_get_by_name(GST_BIN(recording),"microphone-gain");
            g_object_set(gain,"volume",microphoneGain,nullptr);gst_object_unref(gain);
        }
        GstElement *output = gst_bin_get_by_name(GST_BIN(recording), "output");
        g_object_set(output, "location", (path + ".part").toUtf8().constData(), nullptr);
        gst_object_unref(output);
        recordSource = gst_bin_get_by_name(GST_BIN(recording), "record-source");
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB",
            "width", G_TYPE_INT, image.width(), "height", G_TYPE_INT, image.height(),
            "framerate", GST_TYPE_FRACTION, actualFps, 1, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(recordSource), caps); gst_caps_unref(caps);
        if (gst_element_set_state(recording, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            error = i18n("No se pudo iniciar la grabación.");
            gst_element_set_state(recording, GST_STATE_NULL);
            gst_object_unref(recordSource); recordSource = nullptr;
            gst_object_unref(recording); recording = nullptr;
            return false;
        }
        return true;
    }
    bool endVideo(QString &error) {
        GstElement *finishing;
        QString path;
        {
            std::lock_guard<std::mutex> lock(recordingMutex);
            if (!recording) return true;
            finishing = recording; recording = nullptr;
            gst_app_src_end_of_stream(GST_APP_SRC(recordSource));
            gst_object_unref(recordSource); recordSource = nullptr;
            path = pendingPath;
            error = recordedError;
        }
        // Also terminate the live microphone branch so mp4mux can finalize.
        gst_element_send_event(finishing, gst_event_new_eos());
        GstBus *bus = gst_element_get_bus(finishing);
        GstMessage *message = gst_bus_timed_pop_filtered(bus, 8 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && error.isEmpty();
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) error = gstError(message);
        if (!message) error = i18n("No se pudo finalizar el vídeo. Se conserva el archivo .part.");
        if (message) gst_message_unref(message);
        gst_object_unref(bus);
        gst_element_set_state(finishing, GST_STATE_NULL); gst_object_unref(finishing);
        if (ok && !QFile::rename(path + ".part", path)) {
            error = i18n("No se pudo guardar el vídeo. Se conserva el archivo .part."); return false;
        }
        if (ok) {
            QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbnails";
            QDir().mkpath(cache);
            videoPoster.scaled(224,132,Qt::KeepAspectRatio,Qt::SmoothTransformation)
                .save(cache + "/" + QFileInfo(path).fileName() + ".jpg", "JPG", 85);
        }
        return ok;
    }
    void stop() {
        QString error; endVideo(error);
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline); pipeline = nullptr;
        }
        std::lock_guard<std::mutex> lock(frameMutex); latest = QImage();
    }
};

class Preview : public QWidget {
public:
    QImage image;
    QString overlay, caption;
    bool flash = false;
    explicit Preview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 270); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.fillRect(rect(), QColor(0,0,0,45));
        if (!image.isNull()) {
            QSize size = image.size(); size.scale(this->size(), Qt::KeepAspectRatio);
            QRect target(QPoint((width()-size.width())/2, (height()-size.height())/2), size);
            p.drawImage(target, image);
        } else {
            p.setPen(QColor("#a0a0a0")); p.setFont(QFont("sans-serif", 13));
            p.drawText(rect().adjusted(30,30,-30,-30), Qt::AlignCenter | Qt::TextWordWrap,
                       caption.isEmpty() ? i18n("Conectando la cámara…") : caption);
        }
        if (!overlay.isEmpty()) {
            p.setPen(Qt::white); p.setFont(QFont("sans-serif", overlay.size()<3 ? 80 : 20, QFont::DemiBold));
            p.drawText(rect(), Qt::AlignCenter, overlay);
        }
        if (flash) p.fillRect(rect(), QColor(255,255,255,195));
    }
};

static QIcon icon(const QString &kind, QColor color = QColor("#dedede")) {
    QPixmap pix(28,28); pix.fill(Qt::transparent); QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(color,1.7)); p.setBrush(Qt::NoBrush);
    if (kind == "photo") {
        p.drawRoundedRect(QRectF(3,8,22,15),3,3); p.drawEllipse(QPointF(14,15.5),4.5,4.5);
        p.drawLine(8,8,10,5); p.drawLine(10,5,17,5); p.drawLine(17,5,20,8);
    } else if (kind == "video") {
        p.drawRoundedRect(QRectF(3,7,15,15),2,2); QPolygonF polygon;
        polygon << QPointF(18,12) << QPointF(25,8) << QPointF(25,22) << QPointF(18,18); p.drawPolygon(polygon);
    } else if (kind == "burst") {
        for (int x : {4,16}) for (int y : {4,16}) p.drawRoundedRect(QRectF(x,y,8,8),1,1);
    } else if (kind == "settings") {
        for (int y : {7,14,21}) p.drawLine(3,y,25,y);
        p.setBrush(QColor("#303030")); p.drawEllipse(QPointF(10,7),2.5,2.5);
        p.drawEllipse(QPointF(19,14),2.5,2.5); p.drawEllipse(QPointF(8,21),2.5,2.5);
    } else if (kind == "back") {
        p.drawLine(19,5,10,14); p.drawLine(10,14,19,23);
    }
    return QIcon(pix);
}


class Booth : public QWidget {
public:
    Camera camera;
    Preview *preview;
    Preview *video;
    QVideoSink playbackSink;
    QWidget *playback;
    QToolButton *playPause;
    QSlider *seek;
    QLabel *playTime;
    QMediaPlayer player;
    QAudioOutput audioOutput;
    QMediaDevices mediaDevices;
    QTemporaryDir audioTestFiles;
    QStackedWidget *view;
    QListWidget *gallery;
    QPushButton *shutter;
    QToolButton *options, *effects, *back;
    QButtonGroup *modeGroup;
    QTimer timer, countdown;
    QElapsedTimer recordClock, fpsClock, startupClock;
    QSettings settings{"Samera", "Samera"};
    QFileSystemWatcher watcher;
    bool test, recording = false, reviewing = false;
    bool playbackPausedForMinimize=false;
    bool uiSuspended=false;
    int delay = 3, remaining = 0, mode = 1, burstLeft = 0;
    QList<QImage> burstFrames;
    unsigned long lastFrames=0;
    double fps=0;
    int backgroundOpacity=204;
    QString notice;
    qint64 noticeUntil=0;
    explicit Booth(bool synthetic=false) : test(synthetic) {
        setWindowTitle("Samera"); setWindowIcon(icon("photo"));
        setWindowFlags(Qt::Window);
        setAttribute(Qt::WA_TranslucentBackground);
        resize(960,760); setMinimumSize(640,530);
        setStyleSheet("QWidget{background:transparent;color:#ddd;font-family:'Noto Sans';font-size:12px;} QToolButton,QPushButton{border:1px solid #666;border-radius:5px;background:rgba(100,100,100,70);padding:6px 10px;} QToolButton:hover,QPushButton:hover{background:rgba(140,140,140,100);} QToolButton:checked{background:rgba(160,160,160,110);border-color:#858585;} QToolButton:disabled,QPushButton:disabled{color:#777;} QMenu{background:#303030;border:1px solid #555;padding:5px;} QMenu::item{padding:7px 22px;} QMenu::item:selected{background:#526b91;border-radius:3px;} QListWidget{background:rgba(0,0,0,40);border:0;} QListWidget::item{padding:4px;border-radius:4px;} QListWidget::item:selected{background:#526b91;} QScrollBar:horizontal{height:6px;background:transparent;} QScrollBar::handle:horizontal{background:#666;border-radius:3px;} QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}");
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(1,1,1,1); layout->setSpacing(0);
        auto *toolbar=new QWidget(this);auto *toolbarRow=new QHBoxLayout(toolbar);
        toolbarRow->setContentsMargins(8,3,8,3);toolbarRow->addStretch();
        options=new QToolButton(toolbar);options->setText(i18n("Opciones"));options->setIcon(icon("settings"));
        options->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);options->setPopupMode(QToolButton::InstantPopup);
        toolbarRow->addWidget(options);layout->addWidget(toolbar);
        preview = new Preview(this); video = new Preview(this);
        playback=new QWidget(this);auto *playbackLayout=new QVBoxLayout(playback);playbackLayout->setContentsMargins(0,0,0,0);playbackLayout->setSpacing(0);
        playbackLayout->addWidget(video,1);auto *playbackControls=new QWidget(playback);auto *playbackRow=new QHBoxLayout(playbackControls);
        playPause=new QToolButton(playbackControls);playPause->setText("Ⅱ");playPause->setToolTip(i18n("Pausar / reproducir"));
        seek=new QSlider(Qt::Horizontal,playbackControls);playTime=new QLabel("00:00",playbackControls);
        playbackRow->addWidget(playPause);playbackRow->addWidget(seek,1);playbackRow->addWidget(playTime);playbackLayout->addWidget(playbackControls);
        auto *volumeLabel=new QLabel(i18n("Volumen"),playbackControls);volumeLabel->setObjectName("playback-volume-label");auto *volume=new QSlider(Qt::Horizontal,playbackControls);
        volume->setRange(0,100);volume->setFixedWidth(90);volume->setValue(qRound(settings.value("playbackVolume",1.0).toFloat()*100));
        playbackRow->addWidget(volumeLabel);playbackRow->addWidget(volume);
        connect(volume,&QSlider::valueChanged,this,[this](int value){audioOutput.setVolume(value/100.0f);settings.setValue("playbackVolume",value/100.0);});
        connect(playPause,&QToolButton::clicked,this,[this]{if(player.playbackState()==QMediaPlayer::PlayingState)player.pause();else player.play();});
        connect(seek,&QSlider::sliderMoved,&player,[this](int position){player.setPosition(position);});
        connect(&player,&QMediaPlayer::durationChanged,seek,[this](qint64 duration){seek->setRange(0,int(qMin(duration,qint64(INT_MAX))));});
        connect(&player,&QMediaPlayer::positionChanged,seek,[this](qint64 position){if(!seek->isSliderDown())seek->setValue(int(position));playTime->setText(QTime(0,0).addSecs(int(position/1000)).toString("mm:ss"));});
        connect(&player,&QMediaPlayer::playbackStateChanged,playPause,[this](QMediaPlayer::PlaybackState state){playPause->setText(state==QMediaPlayer::PlayingState?"Ⅱ":"▶");});
        connect(&player,&QMediaPlayer::errorOccurred,this,[this](QMediaPlayer::Error,const QString &error){status(i18n("No se pudo reproducir: ")+error);});
        view = new QStackedWidget(this); view->addWidget(preview); view->addWidget(playback); layout->addWidget(view,1);
        player.setVideoSink(&playbackSink); player.setAudioOutput(&audioOutput);
        applyAudioOutput();
        connect(&mediaDevices,&QMediaDevices::audioOutputsChanged,this,[this]{applyAudioOutput();});
        connect(&playbackSink,&QVideoSink::videoFrameChanged,this,[this](const QVideoFrame &frame){
            if(!frame.isValid()||isMinimized()||!isVisible())return;
            video->image=frame.toImage();video->update();
        });
        auto *controls = new QWidget(this); controls->setFixedHeight(58);
        controls->setStyleSheet("QWidget{background:transparent;color:#ddd;} QToolButton{background:rgba(120,120,120,65);border:0;border-radius:4px;padding:3px 10px;} QToolButton:checked{background:rgba(170,170,170,100);} QToolButton:hover{background:rgba(150,150,150,100);} QToolButton::menu-indicator{image:none;}");
        auto *row = new QHBoxLayout(controls); row->setContentsMargins(16,6,16,6);
        auto *left = new QWidget(controls); left->setFixedWidth(190); auto *modes = new QHBoxLayout(left); modes->setContentsMargins(0,0,0,0); modes->setSpacing(0);
        back = new QToolButton(left); back->setIcon(icon("back")); back->setToolTip(i18n("Volver a la cámara")); back->hide(); modes->addWidget(back);
        modeGroup = new QButtonGroup(this);
        for(int i=0;i<3;++i) {
            auto *button = new QToolButton(left); button->setCheckable(true); button->setFixedSize(46,32);
            button->setIcon(icon(QStringList{"burst","photo","video"}[i])); button->setIconSize(QSize(24,24));
            button->setToolTip(QStringList{i18n("Cuatro fotos"), i18n("Foto"), i18n("Vídeo")}[i]);
            modeGroup->addButton(button,i); modes->addWidget(button);
        }
        modeGroup->button(1)->setChecked(true); modes->addStretch(); row->addWidget(left); row->addStretch();
        shutter = new QPushButton(controls); shutter->setFixedSize(44,44); shutter->setToolTip(i18n("Tomar foto · Espacio"));
        row->addWidget(shutter); row->addStretch();
        auto *right = new QWidget(controls); right->setFixedWidth(190); auto *rightLayout = new QHBoxLayout(right); rightLayout->setContentsMargins(0,0,0,0); rightLayout->addStretch();
        effects = new QToolButton(right); effects->setText(i18n("Efectos"));effects->setPopupMode(QToolButton::InstantPopup);
        auto *effectsMenu=new TransparentMenu(effects);auto *effectGroup=new QActionGroup(effectsMenu);
        for(int i=0;i<3;++i){auto *action=effectsMenu->addAction(QStringList{i18n("Normal"),i18n("Blanco y negro"),i18n("Sepia")}[i]);action->setCheckable(true);action->setChecked(i==0);effectGroup->addAction(action);connect(action,&QAction::triggered,this,[this,i]{camera.effect=i;});}
        effects->setMenu(effectsMenu);rightLayout->addWidget(effects); row->addWidget(right); layout->addWidget(controls);
        gallery = new QListWidget(this); gallery->setFixedHeight(103); gallery->setFlow(QListView::LeftToRight);
        gallery->setWrapping(false); gallery->setResizeMode(QListView::Adjust); gallery->setViewMode(QListView::IconMode);
        gallery->setIconSize(QSize(112,66)); gallery->setGridSize(QSize(124,85)); gallery->setSpacing(3);
        gallery->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); gallery->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        gallery->setContextMenuPolicy(Qt::CustomContextMenu); layout->addWidget(gallery);
        QDir().mkpath(SaveDir);
        camera.mirror = settings.value("mirror",true).toBool(); camera.rotation = settings.value("rotation",0).toInt();
        camera.selectedId=settings.value("camera","/dev/video0").toString();
        camera.microphoneId=settings.value("microphone").toString();
        camera.microphoneGain=settings.value("microphoneGain",1.0).toDouble();
        camera.maxHeight=settings.value("maxHeight",4320).toInt();
        camera.requestedFps=settings.value("fps",30).toInt();
        backgroundOpacity=settings.value("opacity",204).toInt();
        delay = settings.value("delay",3).toInt(); makeMenu(); updateShutter(); refreshGallery();
        watcher.addPath(SaveDir);
        connect(&watcher,&QFileSystemWatcher::directoryChanged,this,[this]{refreshGallery();});
        connect(modeGroup,&QButtonGroup::idClicked,this,[this](int id){ if(!recording && !countdown.isActive()){mode=id; live(); updateShutter();}});
        connect(back,&QToolButton::clicked,this,[this]{live();});
        connect(shutter,&QPushButton::clicked,this,[this]{capture();});
        connect(gallery,&QListWidget::itemClicked,this,[this](QListWidgetItem *item){review(item);});
        connect(gallery,&QListWidget::customContextMenuRequested,this,[this](QPoint point){galleryMenu(point);});
        auto *key = new QShortcut(QKeySequence(Qt::Key_Space),this); connect(key,&QShortcut::activated,this,[this]{capture();});
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape),this); connect(escape,&QShortcut::activated,this,[this]{countdown.stop(); burstLeft=0;preview->overlay.clear();live();});
        connect(&countdown,&QTimer::timeout,this,[this]{if(--remaining<=0){countdown.stop();preview->overlay.clear(); performCapture();}else preview->overlay=QString::number(remaining);});
        countdown.setInterval(1000);
        connect(&timer,&QTimer::timeout,this,[this]{tick();}); timer.start(33);
        QString error; if(!camera.start(test,error)) preview->caption=error;
        timer.setInterval(qMax(8,1000/camera.actualFps));
        startupClock.start();fpsClock.start();
    }
    ~Booth() override { timer.stop();countdown.stop();player.stop();camera.stop(); }
    void makeMenu() {
        QMenu *old=options->menu();if(old)old->deleteLater();
        auto *menu = new TransparentMenu(options); options->setMenu(menu);
        auto *donationAction=new QWidgetAction(menu);
        auto *donationContainer=new QWidget(menu);auto *donationLayout=new QVBoxLayout(donationContainer);
        donationLayout->setContentsMargins(8,4,8,16);donationLayout->setSpacing(0);
        auto *donationButton=new QToolButton(donationContainer);donationButton->setText(i18n("Donaciones!"));donationButton->setObjectName("donations-option");
        donationButton->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);donationButton->setFixedHeight(30);
        donationButton->setStyleSheet("QToolButton{color:#d3c6a0;background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #454339,stop:1 #353530);border:1px solid #746d5b;border-radius:5px;padding:4px 10px;font-weight:600;font-size:12px;} QToolButton:hover{color:#e5d8b4;border-color:#a2987c;background:#4b483e;}");
        donationLayout->addWidget(donationButton);
        donationAction->setDefaultWidget(donationContainer);menu->addAction(donationAction);
        connect(donationButton,&QToolButton::clicked,this,[this,menu]{menu->hide();donationsDialog();});
        auto *languageMenu=submenu(menu,i18n("Idioma"));auto *languageGroup=new QActionGroup(menu);
        const QString selectedLanguage=settings.value("language","auto").toString();
        for(const QString &mode:QStringList{"auto","es","en"}){
            auto *action=languageMenu->addAction(mode=="auto"?i18n("Automático (sistema)"):mode=="es"?i18n("Español"):i18n("Inglés"));
            action->setCheckable(true);action->setChecked(mode==selectedLanguage);languageGroup->addAction(action);action->setData(mode);
            connect(action,&QAction::triggered,this,[this,mode]{settings.setValue("language",mode);QTimer::singleShot(0,this,[mode]{setSameraLanguage(mode);});});
        }
        menu->addSeparator();
        auto *cameraMenu=submenu(menu,i18n("Cámara"));auto *cameraGroup=new QActionGroup(menu);
        for(const auto &choice:cameras()){
            auto *action=cameraMenu->addAction(choice.label);action->setCheckable(true);action->setChecked(choice.id==camera.selectedId);cameraGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,id=choice.id]{camera.selectedId=id;settings.setValue("camera",id);restartCamera();});
        }
        if(cameraGroup->actions().isEmpty())cameraMenu->addAction(i18n("No se detectan cámaras"))->setEnabled(false);
        auto *microphoneMenu=submenu(menu,i18n("Micrófono"));auto *microphoneGroup=new QActionGroup(menu);
        for(const auto &choice:microphones()){
            auto *action=microphoneMenu->addAction(choice.label);action->setCheckable(true);action->setChecked(choice.id==camera.microphoneId);microphoneGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,id=choice.id]{camera.microphoneId=id;settings.setValue("microphone",id);});
        }
        auto *outputMenu=submenu(menu,i18n("Salida de sonido"));auto *outputGroup=new QActionGroup(menu);
        auto *systemOutput=outputMenu->addAction(i18n("Predeterminada del sistema"));systemOutput->setCheckable(true);outputGroup->addAction(systemOutput);
        systemOutput->setChecked(settings.value("audioOutput").toByteArray().isEmpty());
        connect(systemOutput,&QAction::triggered,this,[this]{settings.setValue("audioOutput",QByteArray());applyAudioOutput();});
        for(const QAudioDevice &device:QMediaDevices::audioOutputs()){
            auto *action=outputMenu->addAction(device.description());action->setCheckable(true);outputGroup->addAction(action);
            action->setChecked(device.id()==settings.value("audioOutput").toByteArray());
            connect(action,&QAction::triggered,this,[this,id=device.id()]{settings.setValue("audioOutput",id);applyAudioOutput();});
        }
        auto *testAudio=menu->addAction(i18n("Probar micrófono y sonido…"));connect(testAudio,&QAction::triggered,this,[this]{testAudioDialog();});
        auto *gainMenu=submenu(menu,i18n("Volumen del micrófono"));auto *gainGroup=new QActionGroup(menu);
        for(int percent:{50,100,150,200}){
            auto *action=gainMenu->addAction(QString::number(percent)+" %");action->setCheckable(true);action->setChecked(qAbs(camera.microphoneGain-percent/100.0)<0.01);gainGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,percent]{camera.microphoneGain=percent/100.0;settings.setValue("microphoneGain",camera.microphoneGain);});
        }
        auto *qualityMenu=submenu(menu,i18n("Calidad de captura"));auto *qualityGroup=new QActionGroup(menu);
        for(int height:{4320,2160,1080,720}){
            auto *action=qualityMenu->addAction(height==4320?i18n("Máxima disponible"):QString::number(height)+"p");action->setCheckable(true);action->setChecked(height==camera.maxHeight);qualityGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,height]{camera.maxHeight=height;settings.setValue("maxHeight",height);restartCamera();});
        }
        auto *rateMenu=submenu(menu,i18n("Fluidez"));auto *rateGroup=new QActionGroup(menu);
        for(int rate:{30,60}){
            auto *action=rateMenu->addAction(QString::number(rate)+i18n(" fps, si la cámara lo admite"));action->setCheckable(true);action->setChecked(rate==camera.requestedFps);rateGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,rate]{camera.requestedFps=rate;settings.setValue("fps",rate);restartCamera();});
        }
        menu->addSeparator();
        auto *mirror = menu->addAction(i18n("Reflejar horizontalmente"));mirror->setCheckable(true);mirror->setChecked(camera.mirror.load());
        connect(mirror,&QAction::toggled,this,[this](bool checked){camera.mirror=checked;settings.setValue("mirror",checked);});
        auto *rotate = submenu(menu,i18n("Girar imagen"));auto *rotationGroup = new QActionGroup(menu);
        for(int angle:{0,90,180,270}){
            auto *action=rotate->addAction(angle ? QString::number(angle)+"°" : i18n("Sin giro"));action->setCheckable(true);action->setChecked(angle==camera.rotation.load());rotationGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,angle]{camera.rotation=angle;settings.setValue("rotation",angle);});
        }
        auto *timing=submenu(menu,i18n("Temporizador"));auto *delayGroup=new QActionGroup(menu);
        for(int seconds:{0,3,5,10}){
            auto *action=timing->addAction(seconds ? QString::number(seconds)+i18n(" segundos") : i18n("Sin temporizador"));action->setCheckable(true);action->setChecked(delay==seconds);delayGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,seconds]{delay=seconds;settings.setValue("delay",seconds);});
        }
        auto *microphone=menu->addAction(i18n("Grabar sonido"));microphone->setCheckable(true);microphone->setChecked(settings.value("audio",true).toBool());
        connect(microphone,&QAction::toggled,this,[this](bool checked){settings.setValue("audio",checked);});
        auto *opacityMenu=submenu(menu,i18n("Transparencia del fondo"));auto *opacityGroup=new QActionGroup(menu);
        for(int percent:{30,45,65,80,100}){
            auto *action=opacityMenu->addAction(percent==100?i18n("Opaco"):QString::number(100-percent)+i18n(" % transparente"));action->setCheckable(true);
            action->setChecked(qAbs(backgroundOpacity-percent*255/100)<6);opacityGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,percent]{backgroundOpacity=percent*255/100;settings.setValue("opacity",backgroundOpacity);update();});
        }
        menu->addSeparator();auto *folder=menu->addAction(i18n("Abrir carpeta de capturas"));
        connect(folder,&QAction::triggered,this,[]{QDesktopServices::openUrl(QUrl::fromLocalFile(SaveDir));});
        auto *retry=menu->addAction(i18n("Reconectar cámara"));connect(retry,&QAction::triggered,this,[this]{restartCamera();});
        auto *refresh=menu->addAction(i18n("Buscar cámaras y micrófonos"));connect(refresh,&QAction::triggered,this,[this]{QTimer::singleShot(0,this,[this]{makeMenu();status(i18n("Dispositivos actualizados"));});});
        menu->addSeparator();auto *about=menu->addAction(i18n("Acerca de Samera"));
        connect(about,&QAction::triggered,this,[this]{
            QMessageBox::about(this,i18n("Acerca de Samera"),"<h2>Samera</h2><p>"+i18n("Creado por xSayer.")+"</p><p>"+i18n("Software gratis y de código abierto (licencia MIT).")+"</p><p>"+i18n("Para cualquier duda o comentario, puedes hablarme en Discord: @xsayer")+"</p>");
        });
    }
    void donationsDialog(){
        auto *dialog=new QDialog(this);dialog->setObjectName("donations-dialog");dialog->setWindowTitle(i18n("Donaciones!"));dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAutoFillBackground(true);
        QPalette palette=dialog->palette();palette.setColor(QPalette::Window,QColor("#161616"));palette.setColor(QPalette::WindowText,QColor("#eeeeee"));dialog->setPalette(palette);
        dialog->setStyleSheet("QDialog{background:#161616;} QLabel{color:#eee;} QPushButton{color:#e5d8b4;background:#454339;border:1px solid #8b8064;border-radius:6px;padding:9px 18px;font-weight:600;} QPushButton:hover{color:#fff0c8;background:#575142;border-color:#b8a77e;}");
        auto *layout=new QVBoxLayout(dialog);layout->setContentsMargins(24,24,24,24);layout->setSpacing(18);
        auto *title=new QLabel(i18n("Donaciones!"),dialog);title->setObjectName("donations-title");title->setStyleSheet("color:#d8c18a;font-size:22px;font-weight:600;");layout->addWidget(title);
        auto *message=new QLabel(i18n("Samera es gratis y de código abierto. Si te gustaría donar, tu apoyo me ayudaría muchísimo. ¡Gracias por apoyar el proyecto!"),dialog);
        message->setObjectName("donations-message");message->setWordWrap(true);layout->addWidget(message);
        auto *credit=new QLabel("xSayer · Discord: @xsayer",dialog);credit->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(credit);
        auto *button=new QPushButton(i18n("Donar con PayPal"),dialog);button->setObjectName("donate-paypal");layout->addWidget(button);
        const QUrl paypal("https://www.paypal.com/paypalme/xsayer");button->setProperty("paypalUrl",paypal);
        connect(button,&QPushButton::clicked,this,[paypal]{QDesktopServices::openUrl(paypal);});
        auto *link=new QLabel(paypal.toString(),dialog);link->setStyleSheet("color:#aaa;font-size:11px;");link->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(link);
        dialog->resize(470,290);dialog->show();
    }
    void retranslateUi(){
        if(!options||!effects)return;
        options->setText(i18n("Opciones"));effects->setText(i18n("Efectos"));back->setToolTip(i18n("Volver a la cámara"));playPause->setToolTip(i18n("Pausar / reproducir"));
        auto *volume=findChild<QLabel *>("playback-volume-label");if(volume)volume->setText(i18n("Volumen"));
        for(int i=0;i<3;++i)modeGroup->button(i)->setToolTip(QStringList{i18n("Cuatro fotos"),i18n("Foto"),i18n("Vídeo")}[i]);
        const auto effectActions=effects->menu()->actions();
        for(int i=0;i<effectActions.size()&&i<3;++i)effectActions[i]->setText(QStringList{i18n("Normal"),i18n("Blanco y negro"),i18n("Sepia")}[i]);
        makeMenu();updateShutter();
        for(int i=0;i<gallery->count();++i)if(gallery->item(i)->data(Qt::UserRole).toString().endsWith(".mp4",Qt::CaseInsensitive))gallery->item(i)->setText(i18n("Vídeo"));
        auto *donations=findChild<QDialog *>("donations-dialog");
        if(donations){
            donations->setWindowTitle(i18n("Donaciones!"));donations->findChild<QLabel *>("donations-title")->setText(i18n("Donaciones!"));
            donations->findChild<QLabel *>("donations-message")->setText(i18n("Samera es gratis y de código abierto. Si te gustaría donar, tu apoyo me ayudaría muchísimo. ¡Gracias por apoyar el proyecto!"));
            donations->findChild<QPushButton *>("donate-paypal")->setText(i18n("Donar con PayPal"));
        }
        preview->update();
    }
    void applyAudioOutput(){
        const QByteArray selected=settings.value("audioOutput").toByteArray();
        QAudioDevice device=QMediaDevices::defaultAudioOutput();
        for(const auto &candidate:QMediaDevices::audioOutputs())if(candidate.id()==selected){device=candidate;break;}
        audioOutput.setDevice(device);audioOutput.setMuted(false);
        audioOutput.setVolume(settings.value("playbackVolume",1.0).toFloat());
    }
    void playTestTone(){
        const QString path=audioTestFiles.filePath("sound-test.wav");
        if(writeTestTone(path)){applyAudioOutput();player.setSource(QUrl::fromLocalFile(path));player.play();}
        else status(i18n("No se pudo crear la prueba de sonido."));
    }
    void testAudioDialog(){
        if(recording)return;
        auto *dialog=new QDialog(this);dialog->setWindowTitle(i18n("Probar audio · Samera"));dialog->setObjectName("audio-test");dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAttribute(Qt::WA_TranslucentBackground);
        dialog->setStyleSheet("QDialog{background:rgba(0,0,0,204);} QLabel{color:#eee;} QProgressBar{border:1px solid #555;border-radius:4px;text-align:center;} QProgressBar::chunk{background:#41b76f;border-radius:3px;}");
        auto *layout=new QVBoxLayout(dialog);QString name=i18n("Predeterminado del sistema");
        for(const auto &device:microphones())if(device.id==camera.microphoneId)name=device.label;
        layout->addWidget(new QLabel(i18n("Micrófono: ")+name,dialog));layout->addWidget(new QLabel(i18n("Habla: la barra debe moverse. Después graba una frase y escúchala."),dialog));
        auto *meter=new QProgressBar(dialog);meter->setRange(0,100);layout->addWidget(meter);
        auto *message=new QLabel(dialog);layout->addWidget(message);
        auto *tone=new QPushButton(i18n("Probar salida de sonido"),dialog);layout->addWidget(tone);
        auto *record=new QPushButton(i18n("Grabar 3 segundos y reproducir"),dialog);record->setObjectName("record-audio-test");layout->addWidget(record);
        auto probe=std::make_shared<AudioProbe>();auto files=std::make_shared<QTemporaryDir>();
        probe->start(camera.microphoneId,camera.microphoneGain);
        auto *meterTimer=new QTimer(dialog);meterTimer->setInterval(100);
        connect(meterTimer,&QTimer::timeout,dialog,[probe,meter,message]{
            double db=probe->db.load();meter->setValue(qBound(0,int((db+60)*100/60),100));meter->setFormat(QString::number(db,'f',1)+" dB");
            const QString error=probe->error();if(!error.isEmpty())message->setText(error);
        });meterTimer->start();
        connect(tone,&QPushButton::clicked,dialog,[this]{playTestTone();});
        connect(record,&QPushButton::clicked,dialog,[this,dialog,probe,files,record,tone,message]{
            const QString path=files->filePath("microphone.wav");
            player.stop();record->setEnabled(false);tone->setEnabled(false);message->setText(i18n("Grabando… habla durante tres segundos."));
            if(!probe->start(camera.microphoneId,camera.microphoneGain,path)){message->setText(i18n("No se pudo iniciar el micrófono."));record->setEnabled(true);tone->setEnabled(true);return;}
            QTimer::singleShot(3000,dialog,[this,probe,path,record,tone,message]{
                const QString error=probe->error();const double peak=probe->maximum.load();probe->stop();
                probe->start(camera.microphoneId,camera.microphoneGain);
                record->setEnabled(true);tone->setEnabled(true);
                if(!error.isEmpty()){message->setText(error);return;}
                message->setText(peak<-45?i18n("El nivel es muy bajo. Acércate al micrófono o aumenta su volumen."):i18n("Reproduciendo tu grabación…"));
                applyAudioOutput();player.setSource(QUrl::fromLocalFile(path));player.play();
            });
        });
        connect(dialog,&QDialog::finished,this,[this,probe,files]{player.stop();probe->stop();});
        dialog->resize(470,260);dialog->show();
    }
    void restartCamera(){
        countdown.stop();burstLeft=0;camera.stop();QString error;
        camera.start(test,error);preview->caption=error;preview->image=QImage();
        timer.setInterval(qMax(8,1000/camera.actualFps));live();startupClock.restart();
        status(error.isEmpty()?i18n("Cámara lista"):error);
    }
    void updateShutter() {
        shutter->setText(recording ? "■" : "");
        shutter->setIcon(recording ? QIcon() : icon(mode==2 ? "video" : "photo",Qt::white));shutter->setIconSize(QSize(28,28));
        shutter->setStyleSheet("QPushButton{background:#ff3b37;border:0;border-radius:22px;color:white;font-size:21px;padding:0;} QPushButton:hover{background:#ff5952;} QPushButton:pressed{background:#be302c;} QPushButton:disabled{background:#b0807e;}");
        shutter->setToolTip(recording ? i18n("Detener grabación · Espacio") : mode==2 ? i18n("Grabar vídeo · Espacio") : i18n("Tomar foto · Espacio"));
    }
    void status(const QString &message) { notice=message;noticeUntil=QDateTime::currentMSecsSinceEpoch()+3500; }
    void live() {
        if(recording)return;
        reviewing=false;player.stop();view->setCurrentWidget(preview);back->hide();gallery->clearSelection();
        if(camera.pipeline&&!isMinimized())gst_element_set_state(camera.pipeline,GST_STATE_PLAYING);
    }
    void tick() {
        if(isMinimized()||!isVisible())return;
        const QString error=camera.pollError();
        if(!error.isEmpty()) { status(error);preview->caption=error;if(recording)finishRecording(); }
        if(!reviewing) preview->image=camera.frame();
        if(fpsClock.elapsed()>=1500){const auto count=camera.frames.load();fps=(count-lastFrames)*1000.0/fpsClock.elapsed();lastFrames=count;fpsClock.restart();}
        if(!countdown.isActive()) {
            if(recording) preview->overlay="● "+QTime(0,0).addSecs(int(recordClock.elapsed()/1000)).toString("mm:ss");
            else preview->overlay.clear();
        }
        if(preview->image.isNull() && startupClock.elapsed()>8000 && preview->caption.isEmpty())
            preview->caption=i18n("No llega imagen de la cámara. Usa Opciones → Reconectar cámara.");
        if(!recording && !countdown.isActive())shutter->setEnabled(!camera.frame().isNull());
        if(!recording)for(auto *button:modeGroup->buttons())button->setEnabled(!countdown.isActive()&&!burstLeft);
        if(QDateTime::currentMSecsSinceEpoch()<noticeUntil)preview->overlay=notice;
        preview->update();
    }
    bool savePhoto(const QImage &image, QString *saved=nullptr) {
        if(image.isNull())return false;
        const QString path=filename("jpg");QSaveFile file(path);
        bool ok=file.open(QIODevice::WriteOnly)&&image.save(&file,"JPG",95)&&file.commit();
        if(!ok){status(i18n("No se pudo guardar la foto: ")+file.errorString());return false;}
        if(saved)*saved=path;
        preview->flash=true;QTimer::singleShot(90,this,[this]{preview->flash=false;});refreshGallery();status(i18n("Foto guardada"));return true;
    }
    void capture() {
        if(recording){finishRecording();return;}
        if(countdown.isActive()||burstLeft)return;
        live();if(camera.frame().isNull()){status(i18n("La cámara todavía no está lista"));return;}
        if(delay){remaining=delay;preview->overlay=QString::number(remaining);noticeUntil=0;countdown.start();}
        else performCapture();
    }
    void performCapture() {
        if(mode==2){
            QString error;
            if(camera.beginVideo(filename("mp4"),settings.value("audio",true).toBool(),error)){
                recording=true;recordClock.start();options->setEnabled(false);effects->setEnabled(false);gallery->setEnabled(false);
                for(auto *button:modeGroup->buttons())button->setEnabled(false);
                updateShutter();
            }else status(error);
        }else if(mode==0){burstFrames.clear();burstLeft=4;burstShot();}
        else savePhoto(camera.frame());
    }
    void burstShot() {
        QImage image=camera.frame();if(image.isNull()){burstLeft=0;return;}
        burstFrames.append(image);preview->flash=true;QTimer::singleShot(80,this,[this]{preview->flash=false;});
        if(--burstLeft>0){QTimer::singleShot(500,this,[this]{if(burstLeft)burstShot();});return;}
        QImage collage(image.width(),image.height(),QImage::Format_RGB888);collage.fill(Qt::black);QPainter p(&collage);
        for(int i=0;i<4;++i)p.drawImage(QRect((i%2)*image.width()/2,(i/2)*image.height()/2,image.width()/2,image.height()/2),burstFrames[i]);
        p.end();savePhoto(collage);burstFrames.clear();
    }
    void finishRecording() {
        recording=false;preview->overlay=i18n("Guardando vídeo…");preview->repaint();
        QString error;bool ok=camera.endVideo(error);
        options->setEnabled(true);effects->setEnabled(true);gallery->setEnabled(true);for(auto *button:modeGroup->buttons())button->setEnabled(true);
        updateShutter();refreshGallery();status(ok ? i18n("Vídeo guardado") : error);
    }
    void refreshGallery() {
        const QString selection=gallery->currentItem()?gallery->currentItem()->data(Qt::UserRole).toString():QString();
        gallery->clear();
        const QFileInfoList files=QDir(SaveDir).entryInfoList({"*.jpg","*.jpeg","*.png","*.mp4"},QDir::Files,QDir::Time);
        gallery->setVisible(!files.isEmpty());
        for(const QFileInfo &file:files){
            QIcon thumbnail;const bool movie=file.suffix().toLower()=="mp4";
            if(movie){
                const QString cached=QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/thumbnails/"+file.fileName()+".jpg";
                thumbnail=QFileInfo::exists(cached)?QIcon(cached):icon("video");
            }
            else {QImageReader reader(file.absoluteFilePath());reader.setAutoTransform(true);reader.setScaledSize(QSize(224,132));thumbnail=QIcon(QPixmap::fromImage(reader.read()));}
            auto *item=new QListWidgetItem(thumbnail,movie ? i18n("Vídeo") : "",gallery);item->setData(Qt::UserRole,file.absoluteFilePath());
            item->setToolTip(file.fileName());if(file.absoluteFilePath()==selection)item->setSelected(true);
        }
    }
    void review(QListWidgetItem *item) {
        if(recording||countdown.isActive()||burstLeft)return;
        const QString path=item->data(Qt::UserRole).toString();reviewing=true;back->show();
        if(camera.pipeline)gst_element_set_state(camera.pipeline,GST_STATE_PAUSED);
        if(path.endsWith(".mp4",Qt::CaseInsensitive)){view->setCurrentWidget(playback);player.setSource(QUrl::fromLocalFile(path));player.play();}
        else{player.stop();view->setCurrentWidget(preview);preview->image=QImage(path);preview->overlay.clear();preview->update();}
    }
    void galleryMenu(QPoint point) {
        auto *item=gallery->itemAt(point);if(!item||recording)return;
        const QString path=item->data(Qt::UserRole).toString();TransparentMenu menu;
        auto *open=menu.addAction(i18n("Abrir con otra aplicación"));auto *trash=menu.addAction(i18n("Mover a la papelera"));
        QAction *choice=menu.exec(gallery->viewport()->mapToGlobal(point));
        if(choice==open)QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        else if(choice==trash){player.stop();if(!QFile::moveToTrash(path))status(i18n("No se pudo mover a la papelera"));live();refreshGallery();}
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        // Clear the complete alpha surface before drawing a new translucent frame.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(),Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setPen(Qt::NoPen);painter.setBrush(QColor(0,0,0,backgroundOpacity));
        painter.drawRoundedRect(rect(),9,9);
    }
    void changeEvent(QEvent *event) override {
        QWidget::changeEvent(event);
        if(event->type()==QEvent::LanguageChange)retranslateUi();
        if(event->type()==QEvent::WindowStateChange)syncWindowVisibility();
    }
    void syncWindowVisibility(){
        const bool suspended=isMinimized()||!isVisible()||(windowHandle()&&!windowHandle()->isExposed());
        if(suspended!=uiSuspended){
            uiSuspended=suspended;
            if(suspended){
                timer.stop();
                playbackPausedForMinimize=player.playbackState()==QMediaPlayer::PlayingState;
                if(playbackPausedForMinimize)player.pause();
            }else{
                timer.start();
                if(playbackPausedForMinimize){playbackPausedForMinimize=false;player.play();}
                update();preview->update();video->update();
            }
            if(camera.pipeline&&!recording)
                gst_element_set_state(camera.pipeline,(suspended||reviewing)?GST_STATE_PAUSED:GST_STATE_PLAYING);
        }
    }
    bool eventFilter(QObject *object,QEvent *event) override {
        if(object==windowHandle()&&event->type()==QEvent::Expose)
            QTimer::singleShot(0,this,[this]{syncWindowVisibility();});
        return QWidget::eventFilter(object,event);
    }
    void hideEvent(QHideEvent *event) override {
        QWidget::hideEvent(event);syncWindowVisibility();timer.stop();
    }
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        if(windowHandle())windowHandle()->installEventFilter(this);
        QTimer::singleShot(0,this,[this]{syncWindowVisibility();if(!uiSuspended)timer.start();update();});
    }
    void closeEvent(QCloseEvent *event) override {
        countdown.stop();burstLeft=0;if(recording)finishRecording();player.stop();camera.stop();event->accept();
    }
};

int main(int argc,char **argv){
    gst_init(&argc,&argv);QApplication app(argc,argv);
    app.setApplicationName("Samera");app.setOrganizationName("Samera");app.setDesktopFileName("samera");
    const QStringList args=app.arguments();const bool test=args.contains("--test-source");
    QSettings preferences("Samera","Samera");setSameraLanguage(preferences.value("language","auto").toString());
    QLockFile instance(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)+"/samera.lock");
    if(!test&&!args.contains("--window-check")&&!instance.tryLock(100)){QMessageBox::information(nullptr,"Samera",i18n("Samera ya está abierta."));return 0;}
    Booth booth(test);
    if(args.contains("--window-check"))booth.setWindowTitle("Prueba de minimizar y restaurar · Samera");
    booth.show();
    if(args.contains("--self-test")){
        QTimer::singleShot(2200,&booth,[&booth,&app]{
            QString path,error;bool photo=booth.savePhoto(booth.camera.frame(),&path);
            fprintf(stderr,"SELFTEST photo=%d frames=%lu\n",photo,booth.camera.frames.load());
            bool started=booth.camera.beginVideo(filename("mp4"),true,error);
            fprintf(stderr,"SELFTEST recording=%d %s\n",started,qPrintable(error));
            QTimer::singleShot(2300,&booth,[&booth,&app,photo,started]{
                QString error;const bool stopped=booth.camera.endVideo(error);
                fprintf(stderr,"SELFTEST finalization=%d %s frames=%lu\n",stopped,qPrintable(error),booth.camera.frames.load());
                booth.camera.rotation=90;
                QTimer::singleShot(700,&booth,[&booth,&app,photo,started,stopped]{
                    QImage rotated=booth.camera.frame();bool portrait=rotated.size()==QSize(1080,1920)&&booth.savePhoto(rotated);
                    QString error;bool video=booth.camera.beginVideo(filename("mp4"),false,error);
                    QTimer::singleShot(1300,&booth,[&booth,&app,photo,started,stopped,portrait,video]{
                        QString error;bool finalized=booth.camera.endVideo(error);
                        booth.camera.rotation=0;booth.camera.effect=2;booth.mode=0;booth.performCapture();
                        QTimer::singleShot(2500,&booth,[&booth,&app,photo,started,stopped,portrait,video,finalized]{
                            booth.refreshGallery();if(booth.gallery->count()){booth.review(booth.gallery->item(0));booth.live();}
                            booth.camera.stop();QString error;bool restarted=booth.camera.start(true,error);
                            QTimer::singleShot(10000,&booth,[&booth,&app,photo,started,stopped,portrait,video,finalized,restarted]{
                                bool ok=photo&&started&&stopped&&portrait&&video&&finalized&&restarted&&booth.camera.frames.load()>400;
                                booth.grab().save("/tmp/samera-ui.png");
                                fprintf(stderr,"SELFTEST result=%d portrait=%d rotated-video=%d restarted=%d gallery=%d frames=%lu fps=%.1f\n",ok,portrait,video&&finalized,restarted,booth.gallery->count(),booth.camera.frames.load(),booth.fps);
                                app.exit(ok?0:1);
                            });
                        });
                    });
                });
            });
        });
    }
    if(args.contains("--smoke-test")){
        QTimer::singleShot(30000,&booth,[&booth,&app]{
            fprintf(stderr,"HARDWARE frames=%lu recent-fps=%.1f size=%dx%d error=%s\n",booth.camera.frames.load(),booth.fps,booth.camera.frame().width(),booth.camera.frame().height(),qPrintable(booth.camera.pollError()));
            app.exit(booth.camera.frames.load()>300&&!booth.camera.frame().isNull()?0:1);
        });
    }
    if(args.contains("--av-check")){
        QTimer::singleShot(2200,&booth,[&booth,&app]{
            booth.mode=2;booth.delay=0;booth.performCapture();
            QTimer::singleShot(3200,&booth,[&booth,&app]{
                const QString path=booth.camera.pendingPath;booth.finishRecording();
                const bool ok=QFileInfo::exists(path)&&QFileInfo(path).size()>0;
                fprintf(stderr,"AVCHECK saved=%d file=%s frames=%lu\n",ok,qPrintable(path),booth.camera.frames.load());
                app.exit(ok?0:1);
            });
        });
    }
    if(args.contains("--device-check")){
        QTimer::singleShot(2000,&booth,[&booth,&app]{
            const auto choices=cameras();const QString original=booth.camera.selectedId;
            fprintf(stderr,"DEVICES cameras=%lld microphones=%lld\n",(long long)choices.size(),(long long)microphones().size());
            QString other;for(const auto &choice:choices)if(choice.id!=original){other=choice.id;break;}
            if(other.isEmpty()){app.exit(1);return;}
            booth.camera.selectedId=other;booth.restartCamera();
            QTimer::singleShot(2200,&booth,[&booth,&app,original]{
                const QImage image=booth.camera.frame();bool switched=!image.isNull();
                fprintf(stderr,"DEVICES switched=%d size=%dx%d\n",switched,image.width(),image.height());
                booth.camera.selectedId=original;booth.restartCamera();
                QTimer::singleShot(2200,&booth,[&booth,&app,switched]{
                    fprintf(stderr,"DEVICES restored=%d size=%dx%d\n",!booth.camera.frame().isNull(),booth.camera.frame().width(),booth.camera.frame().height());
                    app.exit(switched&&!booth.camera.frame().isNull()?0:1);
                });
            });
        });
    }
    if(args.contains("--playback-check")){
        const QFileInfoList files=QDir(SaveDir).entryInfoList({"*.mp4"},QDir::Files,QDir::Time);
        if(files.isEmpty())return 1;
        booth.reviewing=true;booth.view->setCurrentWidget(booth.playback);
        booth.player.setSource(QUrl::fromLocalFile(files.first().absoluteFilePath()));booth.player.play();
        auto decoded=std::make_shared<int>(0);
        QObject::connect(booth.player.videoSink(),&QVideoSink::videoFrameChanged,&booth,[decoded](const QVideoFrame &frame){if(frame.isValid())++*decoded;});
        QTimer::singleShot(1800,&booth,[&booth,&app,decoded]{
            bool ok=*decoded>0&&!booth.video->image.isNull()&&booth.player.error()==QMediaPlayer::NoError;
            fprintf(stderr,"PLAYBACK decoded=%d position=%lld error=%s\n",*decoded,(long long)booth.player.position(),qPrintable(booth.player.errorString()));
            app.exit(ok?0:1);
        });
    }
    if(args.contains("--audio-output-check")){
        auto captured=std::make_shared<AudioProbe>();
        QProcess query;query.start("pactl",{"get-default-sink"});query.waitForFinished(2000);
        const QString sink=QString::fromUtf8(query.readAllStandardOutput()).trimmed();
        captured->start(sink+".monitor",1.0);
        QTimer::singleShot(2800,&booth,[&booth]{booth.playTestTone();});
        QTimer::singleShot(4800,&booth,[&booth,&app,captured]{
            const double peak=captured->maximum.load();const QString error=captured->error();
            fprintf(stderr,"AUDIOOUTPUT monitor-buffers=%lu peak=%.1fdB device=%s error=%s\n",captured->buffers.load(),peak,qPrintable(booth.audioOutput.device().description()),qPrintable(error));
            if(!app.arguments().contains("--av-check"))app.exit(peak>-60&&error.isEmpty()?0:1);
        });
    }
    if(args.contains("--microphone-check")){
        auto captured=std::make_shared<AudioProbe>();captured->start(booth.camera.microphoneId,booth.camera.microphoneGain);
        QTimer::singleShot(4500,&booth,[&booth,&app,captured]{
            const double peak=captured->maximum.load();const QString error=captured->error();
            fprintf(stderr,"MICROPHONE buffers=%lu peak=%.1fdB source=%s error=%s\n",captured->buffers.load(),peak,qPrintable(booth.camera.microphoneId),qPrintable(error));
            if(!app.arguments().contains("--av-check")&&!app.arguments().contains("--audio-output-check"))app.exit(captured->buffers.load()>0&&peak>-90&&error.isEmpty()?0:1);
        });
    }
    if(args.contains("--window-check")){
        auto success=std::make_shared<bool>(true);
        QTimer::singleShot(1800,&booth,[&booth]{booth.grab().save("/tmp/samera-window-controls.png");});
        for(int cycle=0;cycle<3;++cycle){
            QTimer::singleShot(2200+cycle*2000,&booth,[&booth,success]{
                if(QCoreApplication::arguments().contains("--compositor-window-check"))fprintf(stderr,"WINDOWREQUEST minimize\n");
                else booth.showMinimized();
                QTimer::singleShot(250,&booth,[&booth,success]{
                    const bool hidden=booth.isMinimized()||!booth.isVisible()||!booth.windowHandle()->isExposed();
                    fprintf(stderr,"WINDOWSTATE minimized=%d visible=%d exposed=%d timer-active=%d\n",booth.isMinimized(),booth.isVisible(),booth.windowHandle()->isExposed(),booth.timer.isActive());
                    *success=*success&&hidden&&!booth.timer.isActive();
                });
            });
            QTimer::singleShot(3100+cycle*2000,&booth,[&booth,success]{
                if(QCoreApplication::arguments().contains("--compositor-window-check"))fprintf(stderr,"WINDOWREQUEST restore\n");
                else booth.showNormal();
                QTimer::singleShot(250,&booth,[&booth,success]{*success=*success&&!booth.isMinimized()&&booth.timer.isActive();});
            });
        }
        QTimer::singleShot(8500,&booth,[&booth,&app,success]{
            const bool native=!(booth.windowFlags()&Qt::FramelessWindowHint);
            bool ok=*success&&native&&!booth.camera.frame().isNull();
            fprintf(stderr,"WINDOWCHECK cycles=3 timer-stops=%d native-decoration=%d camera-resumed=%d\n",*success,native,!booth.camera.frame().isNull());
            app.exit(ok?0:1);
        });
    }
    if(args.contains("--audio-dialog-check")){
        QTimer::singleShot(700,&booth,[&booth]{
            booth.testAudioDialog();
            auto *record=booth.findChild<QPushButton *>("record-audio-test");if(record)record->click();
        });
        QTimer::singleShot(4900,&booth,[&booth,&app]{
            const bool ok=booth.player.hasAudio()&&booth.player.position()>0&&booth.player.error()==QMediaPlayer::NoError;
            fprintf(stderr,"AUDIODIALOG has-audio=%d position=%lld duration=%lld error=%s\n",booth.player.hasAudio(),(long long)booth.player.position(),(long long)booth.player.duration(),qPrintable(booth.player.errorString()));
            auto *dialog=booth.findChild<QDialog *>("audio-test");if(dialog)dialog->close();
            app.exit(ok?0:1);
        });
    }
    if(args.contains("--ui-copy-check")){
        const bool hadLanguage=preferences.contains("language");const QVariant previousLanguage=preferences.value("language","auto");
        auto triggerLanguage=[&booth](const QString &mode){for(auto *action:booth.options->menu()->findChildren<QAction *>())if(action->data().toString()==mode){action->trigger();return;}};
        QTimer::singleShot(100,&booth,[triggerLanguage]{triggerLanguage("es");});
        QTimer::singleShot(350,&booth,[&booth,&app,triggerLanguage,hadLanguage,previousLanguage]{
            bool spanish=booth.options->text()=="Opciones"&&booth.settings.value("language")=="es";
            booth.donationsDialog();auto *dialog=booth.findChild<QDialog *>("donations-dialog");
            auto *paypal=dialog->findChild<QPushButton *>("donate-paypal");
            bool link=paypal->property("paypalUrl").toUrl()==QUrl("https://www.paypal.com/paypalme/xsayer");
            dialog->grab().save("/tmp/samera-donations-es.png");booth.options->menu()->grab().save("/tmp/samera-options-es.png");
            triggerLanguage("en");
            QTimer::singleShot(250,&booth,[&booth,&app,dialog,spanish,link,hadLanguage,previousLanguage]{
                bool english=booth.options->text()=="Options"&&dialog->windowTitle()=="Donations!"&&booth.settings.value("language")=="en";
                bool detection=spanishForLanguage("auto",QLocale("es_ES"))&&!spanishForLanguage("auto",QLocale("en_US"))&&!spanishForLanguage("auto",QLocale("fr_FR"));
                dialog->grab().save("/tmp/samera-donations-en.png");
                if(hadLanguage)booth.settings.setValue("language",previousLanguage);else booth.settings.remove("language");booth.settings.sync();
                fprintf(stderr,"UICOPY es=%d en=%d automatic=%d paypal=%d credits=%d\n",spanish,english,detection,link,i18n("Creado por xSayer.").contains("xSayer")&&i18n("Para cualquier duda o comentario, puedes hablarme en Discord: @xsayer").contains("@xsayer"));
                app.exit(spanish&&english&&detection&&link?0:1);
            });
        });
    }
    return app.exec();
}
