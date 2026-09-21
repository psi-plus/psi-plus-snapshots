#include "payloadinfo.h"

#include <QByteArray>
#include <QString>

#include <gst/gst.h>

#include <cstdlib>
#include <iostream>

using namespace PsiMedia;

static bool checkEncodingName(const QString &input, const char *expected)
{
    PPayloadInfo info;
    info.id        = 111;
    info.name      = input;
    info.clockrate = 48000;
    info.channels  = 2;

    GstStructure *structure = payloadInfoToStructure(info, QStringLiteral("audio"));
    if (!structure)
        return false;

    const char *encoding = gst_structure_get_string(structure, "encoding-name");
    const bool  ok       = encoding && QByteArray(encoding) == expected;
    gst_structure_free(structure);
    return ok;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    if (!checkEncodingName(QStringLiteral("opus"), "OPUS")) {
        std::cerr << "lowercase opus was not normalized to OPUS\n";
        return EXIT_FAILURE;
    }
    if (!checkEncodingName(QStringLiteral("OpUs"), "OPUS")) {
        std::cerr << "mixed-case OpUs was not normalized to OPUS\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
