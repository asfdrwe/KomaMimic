// SPDX-License-Identifier: GPL-3.0-only
#include "filenames.h"
#include <QCoreApplication>
#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>
#include <memory>
#include <map>

QStringList legacyEncodings() { return {"CP932", "UTF-8", "CP437", "GBK", "CP949", "Big5"}; }
namespace {
using Converter=std::unique_ptr<UConverter,decltype(&ucnv_close)>;
QString converterName(const QString &encoding) {
    QString converter = encoding;
    if (encoding == "CP932") converter = "windows-932";
    else if (encoding == "CP949") converter = "windows-949";
    else if (encoding == "GBK") converter = "windows-936";
    else if (encoding == "Big5") converter = "windows-950";
    return converter;
}
Converter openConverter(const QString &encoding) {
    const auto name=encoding.toLatin1();
    UErrorCode status = U_ZERO_ERROR;
    ++NameDecodeActivity::converterOpens;
    Converter handle(ucnv_open(name.constData(), &status), ucnv_close);
    if (U_FAILURE(status) || !handle) return {nullptr,ucnv_close};
    ucnv_setToUCallBack(handle.get(), UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &status);
    if (U_FAILURE(status)) return {nullptr,ucnv_close};
    return handle;
}
bool convertStrict(UConverter *handle,const QByteArray &bytes,QString &text,QVector<UChar> &buffer) {
    if(!handle)return false;
    UErrorCode status=U_ZERO_ERROR;
    ucnv_resetToUnicode(handle);
    const int needed = ucnv_toUChars(handle, nullptr, 0, bytes.constData(), int(bytes.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) return false;
    status = U_ZERO_ERROR;
    if(buffer.size()<needed+1)buffer.resize(needed+1);
    ucnv_resetToUnicode(handle);
    const int count = ucnv_toUChars(handle, buffer.data(), int(buffer.size()), bytes.constData(), int(bytes.size()), &status);
    if (U_FAILURE(status)) return false;
    text = QString::fromUtf16(reinterpret_cast<const char16_t *>(buffer.constData()), count);
    // Names with embedded NUL/control characters must remain distinguishable.
    for (QChar c : text) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
}
struct NameDecoder::Impl {
    std::map<QString,Converter> converters;
    QVector<UChar> buffer;
};
NameDecoder::NameDecoder()=default;
NameDecoder::~NameDecoder()=default;
bool NameDecoder::decode(const QByteArray &bytes,const QString &encoding,QString &text) {
    if(!m_impl)m_impl=std::make_unique<Impl>();
    const auto name=converterName(encoding);auto found=m_impl->converters.find(name);
    if(found==m_impl->converters.end())found=m_impl->converters.emplace(name,openConverter(name)).first;
    return convertStrict(found->second.get(),bytes,text,m_impl->buffer);
}
bool decodeStrict(const QByteArray &bytes,const QString &encoding,QString &text,NameDecoder *decoder) {
    if(decoder)return decoder->decode(bytes,encoding,text);
    auto handle=openConverter(converterName(encoding));QVector<UChar> buffer;
    return convertStrict(handle.get(),bytes,text,buffer);
}
QString escapedName(const QByteArray &bytes) {
    QString text;
    for (unsigned char b : bytes) {
        if (b >= 32 && b < 127 && b != '\\') text += QChar(b);
        else text += QString("⟦%1⟧").arg(b, 2, 16, QChar('0')).toUpper();
    }
    return text;
}
DecodedName decodeName(const ArchiveName &name, const NamePolicy &policy,NameDecoder *decoder) {
    DecodedName result; result.warning = name.warning;
    if (!policy.force && name.hasDeclaredName) {
        result.text = name.declaredName;
        result.encoding = "Unicode"; result.origin = QCoreApplication::translate("KomaMimic", "明示Unicode");
    } else if (!policy.force && name.invalidUnicode) {
        result.text = escapedName(name.rawName);
        result.encoding = "raw"; result.origin = QCoreApplication::translate("KomaMimic", "不正なUnicode宣言（元byteを表示）");
    } else {
        const QString encoding = !policy.force && !name.nameEncoding.isEmpty() ? name.nameEncoding : policy.legacyEncoding;
        const bool declared = !policy.force && !name.nameEncoding.isEmpty();
        if (!decodeStrict(name.rawName, encoding, result.text,decoder)) {
            result.text = escapedName(name.rawName);
            result.warning += QCoreApplication::translate("KomaMimic", " %1で解釈できないbyteを可視化しています。").arg(encoding);
        }
        result.encoding = encoding;
        result.assumed = !declared;
        result.origin = policy.force ? QCoreApplication::translate("KomaMimic", "利用者による強制再解釈") : declared ? QCoreApplication::translate("KomaMimic", "宣言charset") : QCoreApplication::translate("KomaMimic", "未宣言名の仮定");
    }
    if (!name.rawDirectory.isEmpty()) {
        const QString encoding = !policy.force && !name.directoryEncoding.isEmpty() ? name.directoryEncoding : policy.legacyEncoding;
        const bool declared = !policy.force && !name.directoryEncoding.isEmpty();
        result.assumed |= !declared;
        result.origin += declared ? QCoreApplication::translate("KomaMimic", " / directory:宣言charset") : policy.force ? QCoreApplication::translate("KomaMimic", " / directory:強制再解釈") : QCoreApplication::translate("KomaMimic", " / directory:未宣言名の仮定");
        QString directory;
        bool invalidDirectory = false;
        const auto decodePart = [&](const QByteArray &part) {
            QString decoded;
            if (decodeStrict(part, encoding, decoded,decoder)) return decoded;
            invalidDirectory = true;
            return escapedName(part);
        };
        if (name.lhaDirectory) {
            // Split only the LHA-defined delimiter, never CP932's trail 0x5c.
            if (encoding == "UTF-16LE") {
                QByteArray part;
                for (qsizetype i = 0; i + 1 < name.rawDirectory.size(); i += 2) {
                    if (quint8(name.rawDirectory[i]) == 255 && quint8(name.rawDirectory[i + 1]) == 255) {
                        directory += decodePart(part);
                        directory += '/'; part.clear();
                    } else part += name.rawDirectory.mid(i, 2);
                }
                directory += decodePart(part);
            } else {
                const auto parts = name.rawDirectory.split(char(255));
                for (qsizetype i = 0; i < parts.size(); ++i) {
                    directory += decodePart(parts[i]);
                    if (i + 1 < parts.size()) directory += '/';
                }
            }
        } else directory = decodePart(name.rawDirectory);
        if (invalidDirectory) result.warning += QCoreApplication::translate("KomaMimic", " directoryの解釈できないbyteを可視化しています。");
        result.text.prepend(directory);
        result.encoding += " / dir:" + encoding;
    }
    result.text.replace('\\', '/'); // Only after decoding complete characters.
    return result;
}
