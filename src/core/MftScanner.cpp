#include "MftScanner.h"

#include "Logger.h"
#include "MemoryMonitor.h"

#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <algorithm>
#include <functional>
#include <stack>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

const QStringList MftScanner::DEFAULT_SKIP_PATTERNS = {
    "node_modules",
    "WinSxS"
};

#pragma pack(push, 1)
// NTFS 3.1 MFT record header layout (per Linux NTFS driver ntfs-record.h).
// Field offsets must match exactly or attribute parsing breaks.
typedef struct {
    DWORD Magic;                    // 0x00 "FILE"
    WORD UpdateSequenceOffset;      // 0x04
    WORD UpdateSequenceCount;       // 0x06
    LONGLONG LogFileSequenceNumber; // 0x08
    WORD SequenceNumber;            // 0x10
    WORD HardLinkCount;             // 0x12
    WORD FirstAttributeOffset;      // 0x14
    WORD Flags;                     // 0x16
    DWORD UsedSizeOfMftRecord;      // 0x18 (4 bytes, not 2!)
    DWORD AllocatedSizeOfMftRecord; // 0x1C (4 bytes, not 2!)
    LONGLONG BaseFileRecordSegment; // 0x20 - non-zero for extension records
    WORD NextAttributeId;           // 0x28
} MFT_RECORD_HEADER;

typedef struct {
    DWORD AttributeType;
    DWORD RecordLength;
    BYTE NonResidentFlag;
    BYTE NameLength;
    WORD NameOffset;
    WORD Flags;
    WORD AttributeId;
    union {
        struct {
            DWORD ValueLength;
            WORD ValueOffset;
            BYTE ResidentFlags;
            BYTE Reserved[3];
        } Resident;
        struct {
            LONGLONG LowVcn;
            LONGLONG HighVcn;
            WORD RunOffset;
            BYTE CompressionUnitSize;
            BYTE Reserved[5];
            LONGLONG AllocatedLength;
            LONGLONG DataLength;
            LONGLONG InitializedLength;
            LONGLONG CompressedLength;
        } NonResident;
    };
} ATTRIBUTE_RECORD_HEADER;
#pragma pack(pop)

namespace {

const DWORD MFT_RECORD_SIZE_DEFAULT = 1024;
const DWORD ATTRIBUTE_TYPE_DATA = 0x80;
const DWORD ATTRIBUTE_TYPE_STANDARD_INFORMATION = 0x10;
const DWORD ATTRIBUTE_TYPE_FILE_NAME = 0x30;
const DWORD ATTRIBUTE_TYPE_VOLUME_NAME = 0x60;
const DWORD ATTRIBUTE_TYPE_VOLUME_INFORMATION = 0x70;
const DWORD ATTRIBUTE_TYPE_END = 0xFFFFFFFF;

// Safety cap on USN record count. 5M records covers virtually every real
// disk; reaching the cap emits a warning and stops enumeration with the
// partial results collected so far (graceful degradation, not a crash).
const uint64_t MAX_USN_RECORDS = 5'000'000;

// --- Diagnostics logging ---
// All MftScanner diagnostics now route through the global Logger so the
// whole application writes to a single log file (ncduwin.log). The
// [MftScanner] prefix makes these entries easy to filter.
void logMsg(const QString& msg)
{
    Logger::info("[MftScanner] " + msg);
}

QString hexDump(const BYTE* data, int len)
{
    QString s;
    for (int i = 0; i < len; i++) {
        s += QString("%1 ").arg(data[i], 2, 16, QChar('0')).toUpper();
    }
    return s.trimmed();
}

// A contiguous cluster run of a non-resident attribute.
struct MftRun {
    qint64 startCluster = 0;   // absolute LCN
    qint64 lengthClusters = 0;
};

// Parse NTFS data runs from a non-resident $DATA attribute.
// attrBase: pointer to the start of the attribute header.
// dataRunOffset: byte offset from attrBase to where the run list begins.
// attrEnd: absolute pointer to one past the end of the attribute.
std::vector<MftRun> parseDataRuns(const BYTE* attrBase, int dataRunOffset, const BYTE* attrEnd)
{
    std::vector<MftRun> runs;
    const BYTE* pos = attrBase + dataRunOffset;
    qint64 prevLcn = 0;

    while (pos < attrEnd) {
        BYTE header = *pos;
        if (header == 0) break;

        int lengthSize = header & 0x0F;
        int offsetSize = (header >> 4) & 0x0F;
        pos++;

        if (lengthSize == 0) break;
        if (pos + lengthSize + offsetSize > attrEnd) break;

        // Read run length (unsigned)
        qint64 runLength = 0;
        for (int b = 0; b < lengthSize; b++)
            runLength |= ((qint64)pos[b]) << (b * 8);
        pos += lengthSize;

        // Read run offset (signed, relative to previous LCN)
        qint64 runOffset = 0;
        if (offsetSize > 0) {
            for (int b = 0; b < offsetSize; b++)
                runOffset |= ((qint64)pos[b]) << (b * 8);
            // Sign-extend if the high bit is set
            if (pos[offsetSize - 1] & 0x80) {
                for (int b = offsetSize; b < 8; b++)
                    runOffset |= ((qint64)0xFF) << (b * 8);
            }
            pos += offsetSize;
        } else {
            // Sparse run — skip (no physical clusters)
            continue;
        }

        qint64 absoluteLcn = prevLcn + runOffset;
        prevLcn = absoluteLcn;
        runs.push_back({absoluteLcn, runLength});
    }

    return runs;
}

inline QString normalizePath(const QString& path)
{
    QString p = path;
    p.replace('\\', '/');
    return p;
}

inline QString getDriveLetter(const QString& path)
{
    QFileInfo fi(path);
    return fi.absoluteFilePath().left(2);
}

inline QString getDevicePath(const QString& driveLetter)
{
    return "\\\\.\\" + driveLetter;
}

uint64_t extractMftRecordNumber(uint64_t fileRefNum)
{
    return fileRefNum & 0x0000FFFFFFFFFFFFULL;
}

bool enablePrivilege(LPCWSTR privilegeName, QString& errorMsg)
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        errorMsg = QString("OpenProcessToken failed (error %1)").arg(GetLastError());
        return false;
    }

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        errorMsg = QString("LookupPrivilegeValue failed (error %1)").arg(GetLastError());
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // AdjustTokenPrivileges can return TRUE even if it didn't actually grant the
    // privilege (e.g. the account/token doesn't hold it) -- must check GetLastError.
    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(hToken);

    if (err == ERROR_NOT_ALL_ASSIGNED) {
        errorMsg = "Privilege not held by token, need to run as Administrator";
        return false;
    }
    if (err != ERROR_SUCCESS) {
        errorMsg = QString("AdjustTokenPrivileges failed (error %1)").arg(err);
        return false;
    }
    return true;
}

void applyUsnFixup(BYTE* record, DWORD recordSize)
{
    MFT_RECORD_HEADER* header = reinterpret_cast<MFT_RECORD_HEADER*>(record);
    if (header->UpdateSequenceOffset == 0 || header->UpdateSequenceCount == 0)
        return;

    WORD* usa = reinterpret_cast<WORD*>(record + header->UpdateSequenceOffset);
    WORD usaSignature = usa[0];

    DWORD sectorsPerRecord = recordSize / 512;
    for (DWORD i = 1; i < header->UpdateSequenceCount && i <= sectorsPerRecord; i++) {
        DWORD sectorIndex = i - 1;
        WORD* sectorEnd = reinterpret_cast<WORD*>(record + sectorIndex * 512 + 510);
        if (*sectorEnd == usaSignature) {
            *sectorEnd = usa[i];
        }
    }
}

} // namespace

MftScanner::MftScanner(const QString& rootPath, QObject* parent)
    : QThread(parent), m_rootPath(normalizePath(QFileInfo(rootPath).absoluteFilePath()))
{
    for (const auto& p : DEFAULT_SKIP_PATTERNS)
        m_skipSet.insert(p.toLower());
    m_skipSet.insert(QStringLiteral("winsxs"));
}

void MftScanner::setSkipHeavyDirs(bool skip)
{
    m_skipSet.clear();
    if (skip) {
        for (const auto& p : DEFAULT_SKIP_PATTERNS)
            m_skipSet.insert(p.toLower());
    }
    m_skipSet.insert(QStringLiteral("winsxs"));
}

void MftScanner::cancel()
{
    m_cancel = true;
}

bool MftScanner::isSupported(const QString& path)
{
    QString devicePath = getDevicePath(getDriveLetter(path));

    HANDLE hVolume = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (hVolume == INVALID_HANDLE_VALUE)
        return false;

    NTFS_VOLUME_DATA_BUFFER volumeData;
    DWORD bytesReturned;
    bool success = DeviceIoControl(
        hVolume,
        FSCTL_GET_NTFS_VOLUME_DATA,
        nullptr, 0,
        &volumeData, sizeof(volumeData),
        &bytesReturned,
        nullptr
    );

    CloseHandle(hVolume);
    return success;
}

HANDLE MftScanner::openVolume(const QString& path)
{
    QString devicePath = getDevicePath(getDriveLetter(path));

    return CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
}

bool MftScanner::getNtfsVolumeData(HANDLE hVolume, NTFS_VOLUME_DATA_BUFFER& data)
{
    DWORD bytesReturned;
    return DeviceIoControl(
        hVolume,
        FSCTL_GET_NTFS_VOLUME_DATA,
        nullptr, 0,
        &data, sizeof(data),
        &bytesReturned,
        nullptr
    ) != 0;
}

bool MftScanner::enumUsnData(HANDLE hVolume, std::vector<MftEntry>& entries, QString& errorMsg)
{
    USN_JOURNAL_DATA journalData;
    DWORD bytesReturned;

    if (!DeviceIoControl(
        hVolume,
        FSCTL_QUERY_USN_JOURNAL,
        nullptr, 0,
        &journalData, sizeof(journalData),
        &bytesReturned,
        nullptr
    )) {
        const DWORD err = GetLastError();
        if (err == ERROR_NOT_FOUND || err == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
            errorMsg = QString("USN Journal not found (error %1)").arg(err);
        } else if (err == ERROR_ACCESS_DENIED) {
            errorMsg = QString("Access denied, need administrator privileges (error %1)").arg(err);
        } else {
            errorMsg = QString("Failed to query USN journal (error %1)").arg(err);
        }
        return false;
    }

    // P1d: Buffer reduced from 64MB to 1MB. The kernel returns USN records
    // in chunks; a smaller user buffer just means more DeviceIoControl calls
    // (1MB holds ~10k records per call — plenty). Saves 63MB peak RSS.
    const DWORD bufferSize = 1 * 1024 * 1024;
    std::vector<BYTE> buffer(bufferSize);
    PUSN_RECORD usnRecord;

    MFT_ENUM_DATA_V0 enumData = {0};
    enumData.StartFileReferenceNumber = 0;
    enumData.LowUsn = 0;
    enumData.HighUsn = journalData.NextUsn;

    uint64_t totalRecords = 0;

    while (!m_cancel) {
        // P0c: poll memory pressure every iteration. If low, stop adding new
        // records and return what we have — partial result beats a crash.
        if (MemoryMonitor::isLowMemory()) {
            m_lowMemory = true;
            emit progress("scanner.warn.low_memory", {});
            logMsg(QString("enumUsnData: low memory at %1 records, stopping early").arg(totalRecords));
            break;
        }

        if (!DeviceIoControl(
            hVolume,
            FSCTL_ENUM_USN_DATA,
            &enumData, sizeof(MFT_ENUM_DATA_V0),
            buffer.data(), bufferSize,
            &bytesReturned,
            nullptr
        )) {
            const DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
                break;
            if (err == ERROR_MORE_DATA)
                continue;
            errorMsg = QString("Failed to enumerate USN data (error %1)").arg(err);
            return false;
        }

        if (bytesReturned <= sizeof(USN))
            break;

        usnRecord = reinterpret_cast<PUSN_RECORD>(buffer.data() + sizeof(USN));

        while (reinterpret_cast<BYTE*>(usnRecord) < buffer.data() + bytesReturned) {
            MftEntry entry;
            entry.fileRefNum = extractMftRecordNumber(usnRecord->FileReferenceNumber);
            entry.parentRefNum = extractMftRecordNumber(usnRecord->ParentFileReferenceNumber);
            entry.name = QString::fromWCharArray(usnRecord->FileName, usnRecord->FileNameLength / sizeof(WCHAR));
            entry.attributes = usnRecord->FileAttributes;
            entry.isDir = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.isSymlink = (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            entry.isHidden = entry.name.startsWith(QLatin1Char('.')) ||
                            entry.name.startsWith(QLatin1Char('$'));
            entry.skipped = shouldSkip(entry.name);

            entries.push_back(entry);
            totalRecords++;

            // P0d: record-count cap. 5M covers virtually all real disks; if
            // we hit it, emit a warning and stop (partial result).
            if (totalRecords >= MAX_USN_RECORDS) {
                emit progress("scanner.warn.record_cap",
                              {{"count", QString::number(totalRecords)}});
                logMsg(QString("enumUsnData: hit MAX_USN_RECORDS cap (%1), stopping").arg(totalRecords));
                emit progress("mft.progress.total_records",
                              {{"count", QString::number(totalRecords)}});
                return true;
            }

            if (totalRecords % 100000 == 0) {
                emit progress("mft.progress.records_scanned",
                              {{"count", QString::number(totalRecords)}});
            }

            usnRecord = reinterpret_cast<PUSN_RECORD>(
                reinterpret_cast<BYTE*>(usnRecord) + usnRecord->RecordLength
            );
        }

        enumData.StartFileReferenceNumber = *reinterpret_cast<USN*>(buffer.data());
    }

    emit progress("mft.progress.total_records",
                  {{"count", QString::number(totalRecords)}});
    return true;
}

bool MftScanner::getFileSizes(HANDLE hVolume, const NTFS_VOLUME_DATA_BUFFER& volData,
                              std::unordered_map<uint64_t, qint64>& sizes, QString& errorMsg)
{
    DWORD mftRecordSize = volData.BytesPerFileRecordSegment;
    if (mftRecordSize == 0)
        mftRecordSize = MFT_RECORD_SIZE_DEFAULT;
    DWORD bytesPerCluster = volData.BytesPerCluster;
    qint64 mftStartLcn = volData.MftStartLcn.QuadPart;
    qint64 mftValidData = volData.MftValidDataLength.QuadPart;

    logMsg(QString("getFileSizes via volume handle: mftRecordSize=%1, bytesPerCluster=%2, MftStartLcn=%3, MftValidDataLength=%4")
               .arg(mftRecordSize).arg(bytesPerCluster).arg(mftStartLcn).arg(mftValidData));

    if (bytesPerCluster == 0 || mftStartLcn < 0) {
        errorMsg = "Invalid NTFS volume data (BytesPerCluster or MftStartLcn)";
        logMsg("ERROR: " + errorMsg);
        return false;
    }

    // --- Step 1: Read MFT record 0 ($MFT's own record) to get its $DATA cluster runs ---
    std::vector<BYTE> record0(mftRecordSize);
    LARGE_INTEGER seekPos;
    seekPos.QuadPart = mftStartLcn * bytesPerCluster;
    DWORD bytesRead = 0;
    if (!SetFilePointerEx(hVolume, seekPos, nullptr, FILE_BEGIN)) {
        errorMsg = QString("SetFilePointerEx to MFT start (LCN %1) failed (error %2)").arg(mftStartLcn).arg(GetLastError());
        logMsg("ERROR: " + errorMsg);
        return false;
    }
    if (!ReadFile(hVolume, record0.data(), mftRecordSize, &bytesRead, nullptr) || bytesRead != mftRecordSize) {
        errorMsg = QString("Failed to read MFT record 0 (bytesRead=%1, error=%2)").arg(bytesRead).arg(GetLastError());
        logMsg("ERROR: " + errorMsg);
        return false;
    }
    logMsg("Read MFT record 0 OK, first16: " + hexDump(record0.data(), 16));

    applyUsnFixup(record0.data(), mftRecordSize);

    // Parse record 0 to find the unnamed $DATA attribute and its cluster runs.
    MFT_RECORD_HEADER* rec0Header = reinterpret_cast<MFT_RECORD_HEADER*>(record0.data());
    if (rec0Header->Magic != 0x454C4946) {
        errorMsg = QString("MFT record 0 magic mismatch: 0x%1").arg(rec0Header->Magic, 8, 16, QChar('0'));
        logMsg("ERROR: " + errorMsg);
        return false;
    }

    BYTE* attrStart = record0.data() + rec0Header->FirstAttributeOffset;
    BYTE* attrEnd = record0.data() + mftRecordSize;
    std::vector<MftRun> runs;

    while (attrStart < attrEnd) {
        ATTRIBUTE_RECORD_HEADER* attr = reinterpret_cast<ATTRIBUTE_RECORD_HEADER*>(attrStart);
        if (attr->AttributeType == ATTRIBUTE_TYPE_END) break;
        if (attr->RecordLength == 0) break;

        if (attr->AttributeType == ATTRIBUTE_TYPE_DATA && attr->NameLength == 0) {
            if (attr->NonResidentFlag != 0) {
                int runOffset = attr->NonResident.RunOffset;
                int attrBaseOffset = (int)(attrStart - record0.data());
                runs = parseDataRuns(attrStart, runOffset, attrStart + attr->RecordLength);
                logMsg(QString("Record 0 $DATA: non-resident, %1 runs, DataLength=%2")
                           .arg(runs.size()).arg(attr->NonResident.DataLength));
            } else {
                logMsg("Record 0 $DATA is resident (unexpected for $MFT)");
            }
            break;
        }
        attrStart += attr->RecordLength;
    }

    if (runs.empty()) {
        errorMsg = "Failed to parse $MFT data runs from record 0";
        logMsg("ERROR: " + errorMsg);
        return false;
    }

    qint64 totalRunClusters = 0;
    for (int i = 0; i < (int)runs.size(); i++) {
        logMsg(QString("  run[%1]: startCluster=%2, lengthClusters=%3").arg(i).arg(runs[i].startCluster).arg(runs[i].lengthClusters));
        totalRunClusters += runs[i].lengthClusters;
    }
    logMsg(QString("  total run clusters: %1 (= %2 bytes)").arg(totalRunClusters).arg(totalRunClusters * bytesPerCluster));

    // --- Step 2: Read all MFT records using the cluster runs ---
    // P1d: Buffer reduced from 16MB to 1MB. 1MB holds 1024 MFT records per
    // ReadFile call (at 1024B/record). More calls, each fast (sequential
    // I/O), but peak RSS drops by 15MB.
    const DWORD bufferSize = 1 * 1024 * 1024;
    std::vector<BYTE> buffer(bufferSize);

    // Diagnostic counters
    uint64_t diagValidMagic = 0;
    uint64_t diagDataAttr = 0;
    uint64_t diagNonZero = 0;
    uint64_t diagTotalRecords = 0;
    uint64_t diagDataFromExtension = 0;  // $DATA sizes filled from extension records
    uint64_t recordNumber = 0;  // MFT record number (sequential across all runs)
    int diagDumped = 0;
    int diagSizeSamples = 0;

    for (const auto& run : runs) {
        if (m_cancel) break;

        qint64 runStartByte = run.startCluster * bytesPerCluster;
        qint64 runLengthBytes = run.lengthClusters * bytesPerCluster;
        qint64 runOffset = 0;

        while (runOffset < runLengthBytes && !m_cancel) {
            // P0c: bail out under memory pressure. Sizes collected so far
            // remain valid; files without a size entry default to 0B.
            if (MemoryMonitor::isLowMemory()) {
                m_lowMemory = true;
                emit progress("scanner.warn.low_memory", {});
                logMsg(QString("getFileSizes: low memory at byte %1, stopping early").arg(runOffset));
                break;
            }

            DWORD toRead = (DWORD)std::min((qint64)bufferSize, runLengthBytes - runOffset);

            seekPos.QuadPart = runStartByte + runOffset;
            if (!SetFilePointerEx(hVolume, seekPos, nullptr, FILE_BEGIN)) {
                errorMsg = QString("SetFilePointerEx failed at byte %1 (error %2)").arg(seekPos.QuadPart).arg(GetLastError());
                logMsg("ERROR: " + errorMsg);
                return false;
            }

            if (!ReadFile(hVolume, buffer.data(), toRead, &bytesRead, nullptr)) {
                errorMsg = QString("ReadFile failed at byte %1 (error %2)").arg(seekPos.QuadPart).arg(GetLastError());
                logMsg("ERROR: " + errorMsg);
                break;
            }
            if (bytesRead == 0) break;

            DWORD recordCount = bytesRead / mftRecordSize;
            for (DWORD i = 0; i < recordCount; i++) {
                BYTE* record = buffer.data() + i * mftRecordSize;
                MFT_RECORD_HEADER* header = reinterpret_cast<MFT_RECORD_HEADER*>(record);
                diagTotalRecords++;

                // Hex-dump the first 3 records to verify magic bytes.
                if (diagDumped < 3) {
                    DWORD magicAsDword = *reinterpret_cast<DWORD*>(record);
                    logMsg(QString("record[%1] first48: %2 | magicDWORD=0x%3 flags=0x%4 firstAttrOff=%5 baseFileRec=0x%6")
                               .arg(recordNumber)
                               .arg(hexDump(record, 48))
                               .arg(magicAsDword, 8, 16, QChar('0'))
                               .arg(header->Flags, 4, 16, QChar('0'))
                               .arg(header->FirstAttributeOffset)
                               .arg((quint64)header->BaseFileRecordSegment, 16, 16, QChar('0')));
                    diagDumped++;
                }

                if (header->Magic != 0x454C4946) {
                    recordNumber++;
                    continue;
                }
                diagValidMagic++;

                if (!(header->Flags & 0x0001)) {
                    recordNumber++;
                    continue;
                }

                // Determine the size-map key for this record.
                // - Base record (BaseFileRecordSegment == 0): use own record number.
                // - Extension record (BaseFileRecordSegment != 0): attribute any
                //   $DATA found here to the BASE record, because USN enumeration
                //   only references base records. Without this, files whose
                //   $DATA lives in an extension record (common for highly
                //   fragmented large files like .zip/.msi/.tgz) would show 0B.
                uint64_t fileRefNum;
                bool isExtensionRecord = false;
                if (header->BaseFileRecordSegment != 0) {
                    fileRefNum = extractMftRecordNumber(
                        (uint64_t)header->BaseFileRecordSegment);
                    isExtensionRecord = true;
                } else {
                    fileRefNum = recordNumber;
                }

                applyUsnFixup(record, mftRecordSize);

                BYTE* aStart = record + header->FirstAttributeOffset;
                BYTE* aEnd = record + mftRecordSize;

                while (aStart < aEnd) {
                    ATTRIBUTE_RECORD_HEADER* attr = reinterpret_cast<ATTRIBUTE_RECORD_HEADER*>(aStart);

                    if (attr->AttributeType == ATTRIBUTE_TYPE_END) break;
                    if (attr->RecordLength == 0) break;

                    if (attr->AttributeType == ATTRIBUTE_TYPE_DATA && attr->NameLength == 0) {
                        qint64 fileSize = 0;
                        QString residency;
                        if (attr->NonResidentFlag == 0) {
                            fileSize = attr->Resident.ValueLength;
                            residency = "resident";
                        } else {
                            fileSize = attr->NonResident.DataLength;
                            residency = QString("non-resident (DataLength=%1, AllocatedLength=%2)")
                                            .arg(attr->NonResident.DataLength)
                                            .arg(attr->NonResident.AllocatedLength);
                        }
                        // Only set if not already known — base record is processed
                        // first (lower record number) and is the authoritative
                        // source. For extension records, this populates files
                        // whose $DATA was split out due to ATTRIBUTE_LIST.
                        if (sizes.find(fileRefNum) == sizes.end()) {
                            sizes[fileRefNum] = fileSize;
                            if (isExtensionRecord)
                                diagDataFromExtension++;
                        }
                        diagDataAttr++;
                        if (fileSize > 0)
                            diagNonZero++;

                        if (diagSizeSamples < 10 && fileSize > 0) {
                            logMsg(QString("  sample: fileRef=%1 size=%2 (%3%4)")
                                       .arg(fileRefNum).arg(fileSize).arg(residency)
                                       .arg(isExtensionRecord ? QStringLiteral(" [from extension]") : QString()));
                            diagSizeSamples++;
                        }
                        break;
                    }

                    aStart += attr->RecordLength;
                }

                recordNumber++;
            }

            runOffset += bytesRead;
        }
    }

    logMsg(QString("=== $MFT parse summary ==="));
    logMsg(QString("  total records scanned: %1").arg(diagTotalRecords));
    logMsg(QString("  valid magic (FILE): %1").arg(diagValidMagic));
    logMsg(QString("  in-use + DATA attr found: %1").arg(diagDataAttr));
    logMsg(QString("  non-zero sizes: %1").arg(diagNonZero));
    logMsg(QString("  $DATA sizes recovered from extension records: %1").arg(diagDataFromExtension));
    logMsg(QString("  sizes map entries: %1").arg(sizes.size()));

    emit progress("mft.progress.mft_parse",
                  {{"valid", QString::number(diagValidMagic)},
                   {"data", QString::number(diagDataAttr)},
                   {"nonzero", QString::number(diagNonZero)},
                   {"total", QString::number(sizes.size())}});

    return true;
}

std::shared_ptr<FileNode> MftScanner::buildTree(std::vector<MftEntry>& entries, const QString& scanPath,
                                                 std::unordered_map<uint64_t, qint64>& sizes)
{
    logMsg(QString("buildTree: entries=%1, sizes map=%2").arg(entries.size()).arg(sizes.size()));

    // P1a: sort entries in place by fileRefNum and binary-search directly,
    // eliminating the separate entryIndex vector (16B × N ≈ 20MB@1.29M).
    std::sort(entries.begin(), entries.end(),
              [](const MftEntry& a, const MftEntry& b) {
                  return a.fileRefNum < b.fileRefNum;
              });

    // Binary-search lookup: returns pointer into the sorted `entries` or nullptr.
    auto findEntry = [&](uint64_t fileRef) -> const MftEntry* {
        auto it = std::lower_bound(entries.begin(), entries.end(), fileRef,
            [](const MftEntry& e, uint64_t r) { return e.fileRefNum < r; });
        if (it != entries.end() && it->fileRefNum == fileRef)
            return &(*it);
        return nullptr;
    };

    std::unordered_map<uint64_t, std::shared_ptr<FileNode>> nodeMap;

    // Count how many entries matched a size, and log sample mismatches.
    uint64_t matchedCount = 0;
    uint64_t unmatchedCount = 0;
    int sampleLogged = 0;

    for (const auto& entry : entries) {
        if (entry.parentRefNum == 0)
            continue;

        // P0c: stop creating new nodes under memory pressure. The partial
        // tree built so far is still usable.
        if (m_lowMemory.load() || MemoryMonitor::isLowMemory()) {
            if (!m_lowMemory.exchange(true)) {
                emit progress("scanner.warn.low_memory", {});
                logMsg("buildTree: low memory during node creation, stopping early");
            }
            break;
        }

        auto node = std::make_shared<FileNode>();
        node->name = entry.name;
        node->nodeType = entry.isDir ? NodeType::Directory : NodeType::File;
        if (entry.isSymlink)
            node->nodeType = NodeType::Symlink;
        node->isHidden = entry.isHidden;
        node->skipped = entry.skipped;

        auto sizeIt = sizes.find(entry.fileRefNum);
        if (sizeIt != sizes.end()) {
            node->size = sizeIt->second;
            matchedCount++;
        } else {
            unmatchedCount++;
            // Log a few unmatched non-directory entries for diagnosis.
            if (sampleLogged < 10 && !entry.isDir) {
                logMsg(QString("  unmatched: fileRef=%1 name=%2 parentRef=%3")
                           .arg(entry.fileRefNum).arg(entry.name).arg(entry.parentRefNum));
                sampleLogged++;
            }
        }

        nodeMap[entry.fileRefNum] = node;
    }

    logMsg(QString("buildTree size matching: matched=%1, unmatched=%2").arg(matchedCount).arg(unmatchedCount));

    // P1a: sizes map is no longer needed — free it now (before path-building
    // and computeSize) to drop ~48B × N ≈ 62MB peak RSS earlier.
    sizes.clear();
    sizes.rehash(0);

    for (const auto& pair : nodeMap) {
        uint64_t fileRef = pair.first;
        auto node = pair.second;

        const MftEntry* entryPtr = findEntry(fileRef);
        if (!entryPtr)
            continue;

        uint64_t parentRef = entryPtr->parentRefNum;
        auto parentIt = nodeMap.find(parentRef);

        if (parentIt != nodeMap.end()) {
            node->parent = parentIt->second;
            parentIt->second->children.push_back(node);
        }
    }

    QString driveLetter = getDriveLetter(scanPath);

    for (auto& pair : nodeMap) {
        uint64_t fileRef = pair.first;
        auto node = pair.second;

        uint64_t currentRef = fileRef;
        std::vector<QString> parts;

        while (currentRef != 5) {
            const MftEntry* entryPtr = findEntry(currentRef);
            if (!entryPtr)
                break;

            parts.push_back(entryPtr->name);
            currentRef = entryPtr->parentRefNum;

            if (currentRef == 0 || currentRef == 5) {
                break;
            }
        }

        std::reverse(parts.begin(), parts.end());

        QString fullPath = driveLetter + "/";
        for (const QString& part : parts) {
            fullPath += part + '/';
        }
        if (!parts.empty())
            fullPath.chop(1);
        node->path = normalizePath(fullPath);
    }

    auto root = std::make_shared<FileNode>();
    QFileInfo fi(scanPath);
    root->name = fi.isRoot() ? fi.path().left(2) : fi.fileName();
    root->path = normalizePath(scanPath);
    root->nodeType = fi.isRoot() ? NodeType::Drive : NodeType::Directory;
    root->isHidden = false;

    uint64_t scanPathRef = 0;
    QString normalizedScanPath = normalizePath(scanPath);
    for (const auto& pair : nodeMap) {
        if (pair.second->path == normalizedScanPath) {
            scanPathRef = pair.first;
            break;
        }
    }

    if (scanPathRef == 0 && fi.isRoot()) {
        scanPathRef = 5;
    }

    for (const auto& pair : nodeMap) {
        uint64_t fileRef = pair.first;
        auto node = pair.second;

        const MftEntry* entryPtr = findEntry(fileRef);
        if (!entryPtr)
            continue;

        if (entryPtr->parentRefNum == scanPathRef) {
            node->parent = root;
            root->children.push_back(node);
            if (node->isDir()) {
                root->dirCount++;
            } else {
                root->fileCount++;
            }
        }
    }

    // P0a-3: iterative post-order size/count aggregation (two-stack).
    // Replaces the recursive std::function lambda which could overflow the
    // C++ stack on deeply nested trees.
    // Stack1 drives the traversal; stack2 holds nodes in post-order
    // (children pushed to s1 before parent is popped from s2).
    std::stack<std::shared_ptr<FileNode>> s1, s2;
    for (auto& child : root->children)
        s1.push(child);
    while (!s1.empty()) {
        auto n = s1.top();
        s1.pop();
        s2.push(n);
        for (auto& c : n->children) {
            if (c)
                s1.push(c);
        }
    }
    // Process in post-order: each directory's children are finalized before
    // the directory itself, so we can safely aggregate from children.
    while (!s2.empty()) {
        auto n = s2.top();
        s2.pop();
        if (!n->isDir())
            continue;
        for (auto& child : n->children) {
            if (!child)
                continue;
            n->size += child->size;
            if (child->isDir()) {
                n->dirCount += 1 + child->dirCount;
                n->fileCount += child->fileCount;
            } else if (child->nodeType == NodeType::File) {
                n->fileCount++;
            }
            // Symlinks contribute size only (already added above), no count.
        }
    }

    // Aggregate root's direct children into root (mirrors the original loop).
    for (auto& child : root->children) {
        root->size += child->size;
        root->dirCount += child->dirCount;
        root->fileCount += child->fileCount;
    }

    return root;
}

bool MftScanner::shouldSkip(const QString& name) const
{
    if (m_skipSet.empty())
        return false;
    return m_skipSet.count(name.toLower()) > 0;
}

bool MftScanner::isReparsePoint(DWORD fileAttributes) const
{
    return (fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void MftScanner::run()
{
    logMsg("=== MftScanner run START ===");
    logMsg("rootPath: " + m_rootPath);
    logMsg("exe dir: " + QCoreApplication::applicationDirPath());
    logMsg("log file: " + Logger::logPath());

    if (!QFileInfo::exists(m_rootPath)) {
        logMsg("ERROR: Path does not exist: " + m_rootPath);
        emit error("mft.err.path_not_found", {{"path", m_rootPath}});
        return;
    }

    QString privError;
    bool privOk = enablePrivilege(SE_BACKUP_NAME, privError);
    logMsg(QString("enablePrivilege(SE_BACKUP_NAME): %1").arg(privOk ? "OK" : ("FAILED - " + privError)));
    if (!privOk) {
        emit progress("mft.warn.privilege_failed", {{"error", privError}});
    }

    HANDLE hVolume = openVolume(m_rootPath);
    logMsg(QString("openVolume: %1 (GetLastError=%2)").arg(hVolume == INVALID_HANDLE_VALUE ? "FAILED" : "OK").arg(GetLastError()));
    if (hVolume == INVALID_HANDLE_VALUE) {
        emit error("mft.err.cannot_open_volume", {{"path", m_rootPath}});
        return;
    }

    try {
        std::vector<MftEntry> entries;

        emit progress("mft.progress.enumerating_usn", {});
        QString usnError;
        if (!enumUsnData(hVolume, entries, usnError)) {
            logMsg("enumUsnData FAILED: " + usnError);
            emit error("mft.err.usn_enum_failed", {{"error", usnError}});
            CloseHandle(hVolume);
            return;
        }
        logMsg(QString("enumUsnData OK: %1 entries").arg(entries.size()));

        NTFS_VOLUME_DATA_BUFFER volumeData;
        bool volDataOk = getNtfsVolumeData(hVolume, volumeData);
        if (volDataOk) {
            logMsg(QString("NTFS volume data: BytesPerFileRecordSegment=%1, BytesPerCluster=%2, NumberSectors=%3, MftStartLcn=%4")
                       .arg(volumeData.BytesPerFileRecordSegment)
                       .arg(volumeData.BytesPerCluster)
                       .arg(volumeData.NumberSectors.QuadPart)
                       .arg(volumeData.MftStartLcn.QuadPart));
        } else {
            logMsg("getNtfsVolumeData FAILED");
            emit error("mft.err.cannot_get_volume_data", {});
            CloseHandle(hVolume);
            return;
        }

        // NOTE: Do NOT close hVolume here — getFileSizes reads $MFT via this handle.

        if (m_cancel) {
            emit error("scanner.err.cancelled", {});
            CloseHandle(hVolume);
            return;
        }

        emit progress("mft.progress.reading_mft", {});
        std::unordered_map<uint64_t, qint64> sizes;
        QString sizeError;
        if (!getFileSizes(hVolume, volumeData, sizes, sizeError)) {
            logMsg("getFileSizes FAILED: " + sizeError);
            emit progress("mft.warn.cannot_read_mft", {{"error", sizeError}});
        }
        logMsg(QString("getFileSizes done: sizes map has %1 entries").arg(sizes.size()));

        // Volume handle no longer needed — raw $MFT reads are done.
        CloseHandle(hVolume);
        hVolume = nullptr;

        if (m_cancel) {
            emit error("scanner.err.cancelled", {});
            return;
        }

        emit progress("mft.progress.building_tree", {});
        auto root = buildTree(entries, m_rootPath, sizes);
        logMsg(QString("buildTree done: root size=%1, children=%2").arg(root->size).arg(root->children.size()));

        if (m_cancel) {
            emit error("scanner.err.cancelled", {});
            return;
        }

        root->sortBySizeDesc();
        emit finishedTree(root);
        logMsg("=== MftScanner run COMPLETE ===");
        logMsg(QString("Log file location: %1").arg(Logger::logPath()));
    } catch (...) {
        if (hVolume)
            CloseHandle(hVolume);
        logMsg("=== MftScanner run EXCEPTION ===");
        emit error("mft.err.scan_failed", {});
    }
}
