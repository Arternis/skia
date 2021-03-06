/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkFontHost_FreeType_common.h"
#include "SkFontDescriptor.h"
#include "SkFontMgr.h"
#include "SkDescriptor.h"
#include "SkOSFile.h"
#include "SkPaint.h"
#include "SkRTConf.h"
#include "SkString.h"
#include "SkStream.h"
#include "SkThread.h"
#include "SkTSearch.h"
#include "SkTypefaceCache.h"
#include "SkTArray.h"

#include <limits>

/** The base SkTypeface implementation for the custom font manager. */
class SkTypeface_Custom : public SkTypeface_FreeType {
public:
    SkTypeface_Custom(const SkFontStyle& style, bool isFixedPitch,
                      bool sysFont, const SkString familyName, int index)
        : INHERITED(style, SkTypefaceCache::NewFontID(), isFixedPitch)
        , fIsSysFont(sysFont), fFamilyName(familyName), fIndex(index)
    { }

    bool isSysFont() const { return fIsSysFont; }

protected:
    void onGetFamilyName(SkString* familyName) const override {
        *familyName = fFamilyName;
    }

    void onGetFontDescriptor(SkFontDescriptor* desc, bool* isLocal) const override {
        desc->setFamilyName(fFamilyName.c_str());
        *isLocal = !this->isSysFont();
    }

    int getIndex() const { return fIndex; }

private:
    const bool fIsSysFont;
    const SkString fFamilyName;
    const int fIndex;

    typedef SkTypeface_FreeType INHERITED;
};

/** The empty SkTypeface implementation for the custom font manager.
 *  Used as the last resort fallback typeface.
 */
class SkTypeface_Empty : public SkTypeface_Custom {
public:
    SkTypeface_Empty() : INHERITED(SkFontStyle(), false, true, SkString(), 0) {}

protected:
    SkStreamAsset* onOpenStream(int*) const override { return NULL; }

private:
    typedef SkTypeface_Custom INHERITED;
};

/** The stream SkTypeface implementation for the custom font manager. */
class SkTypeface_Stream : public SkTypeface_Custom {
public:
    SkTypeface_Stream(const SkFontStyle& style, bool isFixedPitch, bool sysFont,
                      const SkString familyName, SkStreamAsset* stream, int index)
        : INHERITED(style, isFixedPitch, sysFont, familyName, index)
        , fStream(stream)
    { }

protected:
    SkStreamAsset* onOpenStream(int* ttcIndex) const override {
        *ttcIndex = this->getIndex();
        return fStream->duplicate();
    }

private:
    const SkAutoTDelete<const SkStreamAsset> fStream;

    typedef SkTypeface_Custom INHERITED;
};

// This configuration option is useful if we need to open and hold handles to
// all found system font data (e.g., for skfiddle, where the application can't
// access the filesystem to read fonts on demand)

SK_CONF_DECLARE(bool, c_CustomTypefaceRetain, "fonts.customFont.retainAllData", false,
                "Retain the open stream for each found font on the system.");

/** The file SkTypeface implementation for the custom font manager. */
class SkTypeface_File : public SkTypeface_Custom {
public:
    SkTypeface_File(const SkFontStyle& style, bool isFixedPitch, bool sysFont,
                    const SkString familyName, const char path[], int index)
        : INHERITED(style, isFixedPitch, sysFont, familyName, index)
        , fPath(path)
        , fStream(c_CustomTypefaceRetain ? SkStream::NewFromFile(fPath.c_str()) : NULL)
    { }

protected:
    SkStreamAsset* onOpenStream(int* ttcIndex) const override {
        *ttcIndex = this->getIndex();
        if (fStream.get()) {
            return fStream->duplicate();
        } else {
            return SkStream::NewFromFile(fPath.c_str());
        }
    }

private:
    SkString fPath;
    const SkAutoTDelete<SkStreamAsset> fStream;

    typedef SkTypeface_Custom INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

/**
 *  SkFontStyleSet_Custom
 *
 *  This class is used by SkFontMgr_Custom to hold SkTypeface_Custom families.
 */
class SkFontStyleSet_Custom : public SkFontStyleSet {
public:
    explicit SkFontStyleSet_Custom(const SkString familyName) : fFamilyName(familyName) { }

    /** Should only be called during the inital build phase. */
    void appendTypeface(SkTypeface_Custom* typeface) {
        fStyles.push_back().reset(typeface);
    }

    int count() override {
        return fStyles.count();
    }

    void getStyle(int index, SkFontStyle* style, SkString* name) override {
        SkASSERT(index < fStyles.count());
        bool bold = fStyles[index]->isBold();
        bool italic = fStyles[index]->isItalic();
        *style = SkFontStyle(bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight,
                             SkFontStyle::kNormal_Width,
                             italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
        name->reset();
    }

    SkTypeface* createTypeface(int index) override {
        SkASSERT(index < fStyles.count());
        return SkRef(fStyles[index].get());
    }

    static int match_score(const SkFontStyle& pattern, const SkFontStyle& candidate) {
        int score = 0;
        score += (pattern.width() - candidate.width()) * 100;
        score += (pattern.isItalic() == candidate.isItalic()) ? 0 : 1000;
        score += pattern.weight() - candidate.weight();
        return score;
    }

    SkTypeface* matchStyle(const SkFontStyle& pattern) override {
        if (0 == fStyles.count()) {
            return NULL;
        }

        SkTypeface_Custom* closest = fStyles[0];
        int minScore = std::numeric_limits<int>::max();
        for (int i = 0; i < fStyles.count(); ++i) {
            bool bold = fStyles[i]->isBold();
            bool italic = fStyles[i]->isItalic();
            SkFontStyle style = SkFontStyle(bold ? SkFontStyle::kBold_Weight
                                                 : SkFontStyle::kNormal_Weight,
                                            SkFontStyle::kNormal_Width,
                                            italic ? SkFontStyle::kItalic_Slant
                                                   : SkFontStyle::kUpright_Slant);

            int score = match_score(pattern, style);
            if (score < minScore) {
                closest = fStyles[i];
                minScore = score;
            }
        }
        return SkRef(closest);
    }

    SkString getFamilyName() { return fFamilyName; }

private:
    SkTArray<SkAutoTUnref<SkTypeface_Custom>, true> fStyles;
    SkString fFamilyName;

    friend class SkFontMgr_Custom;
};

/**
 *  SkFontMgr_Custom
 *
 *  This class is essentially a collection of SkFontStyleSet_Custom,
 *  one SkFontStyleSet_Custom for each family. This class may be modified
 *  to load fonts from any source by changing the initialization.
 */
class SkFontMgr_Custom : public SkFontMgr {
public:
    typedef SkTArray<SkAutoTUnref<SkFontStyleSet_Custom>, true> Families;
    class SystemFontLoader {
    public:
        virtual ~SystemFontLoader() { }
        virtual void loadSystemFonts(const SkTypeface_FreeType::Scanner&, Families*) const = 0;
    };
    explicit SkFontMgr_Custom(const SystemFontLoader& loader) {
        loader.loadSystemFonts(fScanner, &fFamilies);

        // Try to pick a default font.
        static const char* defaultNames[] = {
            "Arial", "Verdana", "Times New Roman", "Droid Sans", NULL
        };
        for (size_t i = 0; i < SK_ARRAY_COUNT(defaultNames); ++i) {
            SkFontStyleSet_Custom* set = this->onMatchFamily(defaultNames[i]);
            if (NULL == set) {
                continue;
            }

            SkTypeface* tf = set->matchStyle(SkFontStyle(SkFontStyle::kNormal_Weight,
                                                         SkFontStyle::kNormal_Width,
                                                         SkFontStyle::kUpright_Slant));
            if (NULL == tf) {
                continue;
            }

            fDefaultFamily = set;
            break;
        }
        if (NULL == fDefaultFamily) {
            fDefaultFamily = fFamilies[0];
        }
    }

protected:
    int onCountFamilies() const override {
        return fFamilies.count();
    }

    void onGetFamilyName(int index, SkString* familyName) const override {
        SkASSERT(index < fFamilies.count());
        familyName->set(fFamilies[index]->getFamilyName());
    }

    SkFontStyleSet_Custom* onCreateStyleSet(int index) const override {
        SkASSERT(index < fFamilies.count());
        return SkRef(fFamilies[index].get());
    }

    SkFontStyleSet_Custom* onMatchFamily(const char familyName[]) const override {
        for (int i = 0; i < fFamilies.count(); ++i) {
            if (fFamilies[i]->getFamilyName().equals(familyName)) {
                return SkRef(fFamilies[i].get());
            }
        }
        return NULL;
    }

    SkTypeface* onMatchFamilyStyle(const char familyName[],
                                   const SkFontStyle& fontStyle) const override
    {
        SkAutoTUnref<SkFontStyleSet> sset(this->matchFamily(familyName));
        return sset->matchStyle(fontStyle);
    }

    SkTypeface* onMatchFamilyStyleCharacter(const char familyName[], const SkFontStyle&,
                                            const char* bcp47[], int bcp47Count,
                                            SkUnichar character) const override
    {
        return NULL;
    }

    SkTypeface* onMatchFaceStyle(const SkTypeface* familyMember,
                                 const SkFontStyle& fontStyle) const override
    {
        for (int i = 0; i < fFamilies.count(); ++i) {
            for (int j = 0; j < fFamilies[i]->fStyles.count(); ++j) {
                if (fFamilies[i]->fStyles[j] == familyMember) {
                    return fFamilies[i]->matchStyle(fontStyle);
                }
            }
        }
        return NULL;
    }

    SkTypeface* onCreateFromData(SkData* data, int ttcIndex) const override {
        return this->createFromStream(new SkMemoryStream(data), ttcIndex);
    }

    SkTypeface* onCreateFromStream(SkStreamAsset* bareStream, int ttcIndex) const override {
        SkAutoTDelete<SkStreamAsset> stream(bareStream);
        if (NULL == stream || stream->getLength() <= 0) {
            return NULL;
        }

        bool isFixedPitch;
        SkFontStyle style;
        SkString name;
        if (fScanner.scanFont(stream, ttcIndex, &name, &style, &isFixedPitch, NULL)) {
            return SkNEW_ARGS(SkTypeface_Stream, (style, isFixedPitch, false, name,
                                                  stream.detach(), ttcIndex));
        } else {
            return NULL;
        }
    }

    SkTypeface* onCreateFromFile(const char path[], int ttcIndex) const override {
        SkAutoTDelete<SkStreamAsset> stream(SkStream::NewFromFile(path));
        return stream.get() ? this->createFromStream(stream.detach(), ttcIndex) : NULL;
    }

    SkTypeface* onLegacyCreateTypeface(const char familyName[], unsigned styleBits) const override {
        SkTypeface::Style oldStyle = (SkTypeface::Style)styleBits;
        SkFontStyle style = SkFontStyle(oldStyle & SkTypeface::kBold
                                                 ? SkFontStyle::kBold_Weight
                                                 : SkFontStyle::kNormal_Weight,
                                        SkFontStyle::kNormal_Width,
                                        oldStyle & SkTypeface::kItalic
                                                 ? SkFontStyle::kItalic_Slant
                                                 : SkFontStyle::kUpright_Slant);
        SkTypeface* tf = NULL;

        if (familyName) {
            tf = this->onMatchFamilyStyle(familyName, style);
        }

        if (NULL == tf) {
            tf = fDefaultFamily->matchStyle(style);
        }

        return SkSafeRef(tf);
    }

private:
    Families fFamilies;
    SkFontStyleSet_Custom* fDefaultFamily;
    SkTypeface_FreeType::Scanner fScanner;
};

///////////////////////////////////////////////////////////////////////////////

class DirectorySystemFontLoader : public SkFontMgr_Custom::SystemFontLoader {
public:
    DirectorySystemFontLoader(const char* dir) : fBaseDirectory(dir) { }

    void loadSystemFonts(const SkTypeface_FreeType::Scanner& scanner,
                         SkFontMgr_Custom::Families* families) const override
    {
        load_directory_fonts(scanner, fBaseDirectory, ".ttf", families);
        load_directory_fonts(scanner, fBaseDirectory, ".ttc", families);
        load_directory_fonts(scanner, fBaseDirectory, ".otf", families);
        load_directory_fonts(scanner, fBaseDirectory, ".pfb", families);

        if (families->empty()) {
            SkFontStyleSet_Custom* family = new SkFontStyleSet_Custom(SkString());
            families->push_back().reset(family);
            family->appendTypeface(SkNEW(SkTypeface_Empty));
        }
    }

private:
    static SkFontStyleSet_Custom* find_family(SkFontMgr_Custom::Families& families,
                                              const char familyName[])
    {
       for (int i = 0; i < families.count(); ++i) {
            if (families[i]->getFamilyName().equals(familyName)) {
                return families[i].get();
            }
        }
        return NULL;
    }

    static void load_directory_fonts(const SkTypeface_FreeType::Scanner& scanner,
                                     const SkString& directory, const char* suffix,
                                     SkFontMgr_Custom::Families* families)
    {
        SkOSFile::Iter iter(directory.c_str(), suffix);
        SkString name;

        while (iter.next(&name, false)) {
            SkString filename(SkOSPath::Join(directory.c_str(), name.c_str()));
            SkAutoTDelete<SkStream> stream(SkStream::NewFromFile(filename.c_str()));
            if (!stream.get()) {
                SkDebugf("---- failed to open <%s>\n", filename.c_str());
                continue;
            }

            int numFaces;
            if (!scanner.recognizedFont(stream, &numFaces)) {
                SkDebugf("---- failed to open <%s> as a font\n", filename.c_str());
                continue;
            }

            for (int faceIndex = 0; faceIndex < numFaces; ++faceIndex) {
                bool isFixedPitch;
                SkString realname;
                SkFontStyle style = SkFontStyle(); // avoid uninitialized warning
                if (!scanner.scanFont(stream, faceIndex, &realname, &style, &isFixedPitch, NULL)) {
                    SkDebugf("---- failed to open <%s> <%d> as a font\n",
                             filename.c_str(), faceIndex);
                    continue;
                }

                SkTypeface_Custom* tf = SkNEW_ARGS(SkTypeface_File, (
                                                    style,
                                                    isFixedPitch,
                                                    true,  // system-font (cannot delete)
                                                    realname,
                                                    filename.c_str(),
                                                    faceIndex));

                SkFontStyleSet_Custom* addTo = find_family(*families, realname.c_str());
                if (NULL == addTo) {
                    addTo = new SkFontStyleSet_Custom(realname);
                    families->push_back().reset(addTo);
                }
                addTo->appendTypeface(tf);
            }
        }

        SkOSFile::Iter dirIter(directory.c_str());
        while (dirIter.next(&name, true)) {
            if (name.startsWith(".")) {
                continue;
            }
            SkString dirname(SkOSPath::Join(directory.c_str(), name.c_str()));
            load_directory_fonts(scanner, dirname, suffix, families);
        }
    }

    SkString fBaseDirectory;
};

struct SkEmbeddedResource { const uint8_t* data; size_t size; };
struct SkEmbeddedResourceHeader { const SkEmbeddedResource* entries; int count; };

class EmbeddedSystemFontLoader : public SkFontMgr_Custom::SystemFontLoader {
public:
    EmbeddedSystemFontLoader(const SkEmbeddedResourceHeader* header) : fHeader(header) { }

    void loadSystemFonts(const SkTypeface_FreeType::Scanner& scanner,
                         SkFontMgr_Custom::Families* families) const override
    {
        for (int i = 0; i < fHeader->count; ++i) {
            const SkEmbeddedResource& fontEntry = fHeader->entries[i];
            load_embedded_font(scanner, fontEntry.data, fontEntry.size, i, families);
        }

        if (families->empty()) {
            SkFontStyleSet_Custom* family = new SkFontStyleSet_Custom(SkString());
            families->push_back().reset(family);
            family->appendTypeface(SkNEW(SkTypeface_Empty));
        }
    }

private:
    static SkFontStyleSet_Custom* find_family(SkFontMgr_Custom::Families& families,
                                              const char familyName[])
    {
       for (int i = 0; i < families.count(); ++i) {
            if (families[i]->getFamilyName().equals(familyName)) {
                return families[i].get();
            }
        }
        return NULL;
    }

    static void load_embedded_font(const SkTypeface_FreeType::Scanner& scanner,
                                   const uint8_t* data, size_t size, int index,
                                   SkFontMgr_Custom::Families* families)
    {
        SkAutoTDelete<SkMemoryStream> stream(new SkMemoryStream(data, size, false));

        int numFaces;
        if (!scanner.recognizedFont(stream, &numFaces)) {
            SkDebugf("---- failed to open <%d> as a font\n", index);
            return;
        }

        for (int faceIndex = 0; faceIndex < numFaces; ++faceIndex) {
            bool isFixedPitch;
            SkString realname;
            SkFontStyle style = SkFontStyle(); // avoid uninitialized warning
            if (!scanner.scanFont(stream, faceIndex, &realname, &style, &isFixedPitch, NULL)) {
                SkDebugf("---- failed to open <%d> <%d> as a font\n", index, faceIndex);
                return;
            }

            SkTypeface_Custom* tf = SkNEW_ARGS(SkTypeface_Stream, (
                                                style,
                                                isFixedPitch,
                                                true,  // system-font (cannot delete)
                                                realname,
                                                stream.detach(),
                                                faceIndex));

            SkFontStyleSet_Custom* addTo = find_family(*families, realname.c_str());
            if (NULL == addTo) {
                addTo = new SkFontStyleSet_Custom(realname);
                families->push_back().reset(addTo);
            }
            addTo->appendTypeface(tf);
        }
    }

    const SkEmbeddedResourceHeader* fHeader;
};

#ifdef SK_EMBEDDED_FONTS

extern "C" const SkEmbeddedResourceHeader SK_EMBEDDED_FONTS;
SkFontMgr* SkFontMgr::Factory() {
    return new SkFontMgr_Custom(EmbeddedSystemFontLoader(&SK_EMBEDDED_FONTS));
}

#else

#ifndef SK_FONT_FILE_PREFIX
#    define SK_FONT_FILE_PREFIX "/usr/share/fonts/"
#endif
SkFontMgr* SkFontMgr::Factory() {
    return new SkFontMgr_Custom(DirectorySystemFontLoader(SK_FONT_FILE_PREFIX));
}

#endif
