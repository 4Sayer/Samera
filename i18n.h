// SPDX-License-Identifier: MIT
#pragma once
#include <QtCore>

class SameraTranslator final : public QTranslator {
public:
    bool english=true;
    bool isEmpty() const override { return false; }
    QString translate(const char *, const char *source, const char *, int) const override {
        if(!english)return QString();
        static const QHash<QString,QString> strings={
            {"Opciones","Options"},
            {"Cámara","Camera"},
            {"Micrófono","Microphone"},
            {"Salida de sonido","Audio output"},
            {"Predeterminado del sistema","System default"},
            {"Predeterminada del sistema","System default"},
            {"No se detectan cámaras","No cameras detected"},
            {"Probar micrófono y sonido…","Test microphone and sound…"},
            {"Volumen del micrófono","Microphone volume"},
            {"Calidad de captura","Capture quality"},
            {"Máxima disponible","Highest available"},
            {"Fluidez","Frame rate"},
            {" fps, si la cámara lo admite"," fps, if supported by the camera"},
            {"Reflejar horizontalmente","Mirror horizontally"},
            {"Girar imagen","Rotate image"},
            {"Sin giro","No rotation"},
            {"Temporizador","Timer"},
            {" segundos"," seconds"},
            {"Sin temporizador","No timer"},
            {"Grabar sonido","Record audio"},
            {"Transparencia del fondo","Background transparency"},
            {"Opaco","Opaque"},
            {" % transparente"," % transparent"},
            {"Abrir carpeta de capturas","Open captures folder"},
            {"Reconectar cámara","Reconnect camera"},
            {"Buscar cámaras y micrófonos","Refresh cameras and microphones"},
            {"Dispositivos actualizados","Devices refreshed"},
            {"Acerca de Samera","About Samera"},
            {"Pausar / reproducir","Pause / play"},
            {"Volumen","Volume"},
            {"Volver a la cámara","Back to camera"},
            {"Cuatro fotos","Four photos"},
            {"Foto","Photo"},
            {"Vídeo","Video"},
            {"Tomar foto · Espacio","Take photo · Space"},
            {"Grabar vídeo · Espacio","Record video · Space"},
            {"Detener grabación · Espacio","Stop recording · Space"},
            {"Efectos","Effects"},
            {"Blanco y negro","Black and white"},
            {"Normal","Normal"},
            {"Sepia","Sepia"},
            {" · Color"," · Color"},
            {" · Infrarroja"," · Infrared"},
            {"Conectando la cámara…","Connecting to camera…"},
            {"Error de cámara","Camera error"},
            {"No se pudo iniciar el micrófono.","Could not start the microphone."},
            {"La cámara seleccionada no está disponible. Elige otra en Opciones → Cámara.","The selected camera is unavailable. Choose another in Options → Camera."},
            {"No se encuentra la cámara.","Camera not found."},
            {"No se pudo iniciar la cámara.","Could not start the camera."},
            {"La cámara no admite el modo Full HD MJPEG.","The camera does not support Full HD MJPEG."},
            {"No se pudo abrir la cámara. Cierra otras apps que la estén usando.","Could not open the camera. Close other apps that are using it."},
            {"Espera a que aparezca la cámara.","Wait for the camera preview."},
            {"No se pudo iniciar el vídeo.","Could not start video."},
            {"No se pudo iniciar la grabación.","Could not start recording."},
            {"No se pudo finalizar el vídeo. Se conserva el archivo .part.","Could not finalize the video. The .part file has been kept."},
            {"No se pudo guardar el vídeo. Se conserva el archivo .part.","Could not save the video. The .part file has been kept."},
            {"No se pudo reproducir: ","Could not play: "},
            {"No se pudo crear la prueba de sonido.","Could not create the sound test."},
            {"Probar audio · Samera","Test audio · Samera"},
            {"Micrófono: ","Microphone: "},
            {"Habla: la barra debe moverse. Después graba una frase y escúchala.","Speak: the meter should move. Then record a sentence and listen to it."},
            {"Probar salida de sonido","Test audio output"},
            {"Grabar 3 segundos y reproducir","Record 3 seconds and play"},
            {"Grabando… habla durante tres segundos.","Recording… speak for three seconds."},
            {"El nivel es muy bajo. Acércate al micrófono o aumenta su volumen.","The level is very low. Move closer to the microphone or increase its volume."},
            {"Reproduciendo tu grabación…","Playing your recording…"},
            {"Cámara lista","Camera ready"},
            {"No llega imagen de la cámara. Usa Opciones → Reconectar cámara.","No camera image received. Use Options → Reconnect camera."},
            {"No se pudo guardar la foto: ","Could not save the photo: "},
            {"Foto guardada","Photo saved"},
            {"La cámara todavía no está lista","The camera is not ready yet"},
            {"Guardando vídeo…","Saving video…"},
            {"Vídeo guardado","Video saved"},
            {"Abrir con otra aplicación","Open with another app"},
            {"Mover a la papelera","Move to trash"},
            {"No se pudo mover a la papelera","Could not move to trash"},
            {"Samera ya está abierta.","Samera is already open."},
            {"Idioma","Language"},
            {"Automático (sistema)","Automatic (system)"},
            {"Español","Spanish"},
            {"Inglés","English"},
            {"Donaciones!","Donations!"},
            {"Donar con PayPal","Donate with PayPal"},
            {"Samera es gratis y de código abierto. Si te gustaría donar, tu apoyo me ayudaría muchísimo. ¡Gracias por apoyar el proyecto!","Samera is free and open source. If you would like to donate, your support would help me a great deal. Thank you for supporting the project!"},
            {"Creado por xSayer.","Created by xSayer."},
            {"Para cualquier duda o comentario, puedes hablarme en Discord: @xsayer","For questions or feedback, you can contact me on Discord: @xsayer"},
            {"Software gratis y de código abierto (licencia MIT).","Free and open-source software (MIT license)."},
            {"Espacio: capturar · Esc: volver a la cámara","Space: capture · Esc: back to camera"},
        };
        return strings.value(QString::fromUtf8(source));
    }
};
inline SameraTranslator &sameraTranslator(){static SameraTranslator value;return value;}
inline QTranslator &sameraQtTranslator(){static QTranslator value;return value;}
inline QString i18n(const char *source){return QCoreApplication::translate("Samera",source);}
inline bool spanishForLanguage(const QString &mode,const QLocale &system){
    return mode=="es"||(mode=="auto"&&system.language()==QLocale::Spanish);
}
inline QLocale sameraSystemLocale(){
    if(qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE",Qt::CaseInsensitive)){
        QSettings desktop(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/plasma-localerc",QSettings::IniFormat);
        const QString language=desktop.value("Translations/LANGUAGE").toString().section(':',0,0);
        if(!language.isEmpty())return QLocale(language);
    }
    return QLocale::system();
}
inline void setSameraLanguage(const QString &mode){
    auto *app=QCoreApplication::instance();
    app->removeTranslator(&sameraTranslator());app->removeTranslator(&sameraQtTranslator());
    bool spanish=spanishForLanguage(mode,sameraSystemLocale());
    sameraTranslator().english=!spanish;
    QLocale::setDefault(QLocale(spanish?QLocale::Spanish:QLocale::English));
    if(spanish&&sameraQtTranslator().load("qtbase_es",QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app->installTranslator(&sameraQtTranslator());
    app->installTranslator(&sameraTranslator());
}
