// Copyright (C) 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/gps/GpxParser.h"

#include "aemu/base/StringParse.h"

#include <libxml/parser.h>

#include <algorithm>
#include <string.h>
#include <time.h>

using std::string;

// format an error message
template <class... Args>
static string formatError(const char* format, Args&&... args) {
    char buf[100] = {};
    snprintf(buf, sizeof(buf) - 1, format, std::forward<Args>(args)...);
    return buf;
}


static void cleanupXmlDoc(xmlDoc *doc)
{
    xmlFreeDoc(doc);
    xmlCleanupParser();
}

static bool parseLocation(xmlNode *ptNode, xmlDoc *doc, GpsFix *result, string *error)
{
    double latitude;
    double longitude;

    xmlAttrPtr attr;
    xmlChar *tmpStr;

    // Check for and get the latitude attribute
    attr = xmlHasProp(ptNode, (const xmlChar *) "lat");
    if (!attr || !(tmpStr = xmlGetProp(ptNode, (const xmlChar *) "lat"))) {
        *error = formatError("Point missing a latitude on line %d.",
                             ptNode->line);
        return false; // Return error since a point *must* have a latitude
    } else {
        int read =
            android::base::SscanfWithCLocale(reinterpret_cast<const char*>(tmpStr), "%lf", &latitude);
        xmlFree(tmpStr); // Caller-freed
        if (read != 1) {
            return false;
        }
    }

    // Check for and get the longitude attribute
    attr = xmlHasProp(ptNode, (const xmlChar *) "lon");
    if (!attr || !(tmpStr = xmlGetProp(ptNode, (const xmlChar *) "lon"))) {
        *error = formatError("Point missing a longitude on line %d.",
                             ptNode->line);
        return false; // Return error since a point *must* have a longitude
    } else {
        int read =
            android::base::SscanfWithCLocale(reinterpret_cast<const char*>(tmpStr), "%lf", &longitude);
        xmlFree(tmpStr); // Caller-freed
        if (read != 1) {
            return false;
        }
    }

    // The result will be valid if this point is reached
    result->latitude = latitude;
    result->longitude = longitude;

    // Check for potential children nodes (including time, elevation, name, and description)
    // Note that none are actually required according to the GPX format.
    int childCount = 0;
    for (xmlNode *field = ptNode->children; field; field = field->next) {
        tmpStr = nullptr;

        if ( !strcmp((const char *) field->name, "time") ) {
            if ((tmpStr = xmlNodeListGetString(doc, field->children, 1))) {
                // Convert to a number
                struct tm time = {};
                time.tm_isdst = -1;
                int results = sscanf((const char *)tmpStr,
                                     "%u-%u-%uT%u:%u:%u",
                                     &time.tm_year, &time.tm_mon, &time.tm_mday,
                                     &time.tm_hour, &time.tm_min, &time.tm_sec);
                if (results != 6) {
                    *error = formatError(
                                 "Improperly formatted time on line %d.<br/>"
                                 "Times must be in ISO format.", ptNode->line);
                    return false;
                }

                // Correct according to the struct tm specification
                time.tm_year -= 1900; // Years since 1900
                time.tm_mon -= 1; // Months since January, 0-11

                result->time = mktime(&time);
                xmlFree(tmpStr); // Caller-freed
                childCount++;
            }
        }
        else if ( !strcmp((const char *) field->name, "ele") ) {
            if ((tmpStr = xmlNodeListGetString(doc, field->children, 1))) {
                int read =
                    android::base::SscanfWithCLocale(reinterpret_cast<const char*>(tmpStr), "%lf", &result->elevation);
                xmlFree(tmpStr); // Caller-freed
                if (read != 1) {
                    return false;
                }
                childCount++;
            }
        }
        else if ( !strcmp((const char *) field->name, "name") ) {
            if ((tmpStr = xmlNodeListGetString(doc, field->children, 1))) {
                result->name = reinterpret_cast<const char*>(tmpStr);
                xmlFree(tmpStr); // Caller-freed
                childCount++;
            }
        }
        else if ( !strcmp((const char *) field->name, "desc") ) {
            if ((tmpStr = xmlNodeListGetString(doc, field->children, 1))) {
                result->description = reinterpret_cast<const char*>(tmpStr);
                xmlFree(tmpStr); // Caller-freed
                childCount++;
            }
        }

        // We only care about 4 potential child fields, so quit after finding those
        if (childCount == 4) {
            break;
        }
    }

    return true;
}

static bool parse(xmlDoc *doc, GpsFixArray *fixes, string *error)
{
    xmlNode *root = xmlDocGetRootElement(doc);
    GpsFix location;
    bool isOk;

    for (xmlNode *child = root->children; child; child = child->next) {

        // Individual <wpt> elements are parsed on their own
        if ( !strcmp((const char *) child->name, "wpt") ) {
            isOk = parseLocation(child, doc, &location, error);
            if (!isOk) {
                cleanupXmlDoc(doc);
                return false;
            }
            fixes->push_back(location);
        }

        // <rte> elements require an additional depth of parsing
        else if ( !strcmp((const char *) child->name, "rte") ) {
            for (xmlNode *rtept = child->children; rtept; rtept = rtept->next) {

                // <rtept> elements are parsed just like <wpt> elements
                if ( !strcmp((const char *) rtept->name, "rtept") ) {
                    isOk = parseLocation(rtept, doc, &location, error);
                    if (!isOk) {
                        cleanupXmlDoc(doc);
                        return false;
                    }
                    fixes->push_back(location);
                }
            }
        }

        // <trk> elements require two additional depths of parsing
        else if ( !strcmp((const char *) child->name, "trk") ) {
            for (xmlNode *trkseg = child->children; trkseg; trkseg = trkseg->next) {

                // Skip non <trkseg> elements
                if ( !strcmp((const char *) trkseg->name, "trkseg") ) {

                    // <trkseg> elements an additional depth of parsing
                    for (xmlNode *trkpt = trkseg->children; trkpt; trkpt = trkpt->next) {

                        // <trkpt> elements are parsed just like <wpt> elements
                        if ( !strcmp((const char *) trkpt->name, "trkpt") ) {
                            isOk = parseLocation(trkpt, doc, &location, error);
                            if (!isOk) {
                                cleanupXmlDoc(doc);
                                return false;
                            }
                            fixes->push_back(location);
                        }
                    }
                }
            }
        }
    }

    // Sort the values by timestamp
    std::sort(fixes->begin(), fixes->end());

    cleanupXmlDoc(doc);
    return true;
}

bool GpxParser::parseFile(const char *filePath, GpsFixArray *fixes, string *error)
{
    xmlDocPtr doc = xmlReadFile(filePath, nullptr, 0);
    if (doc == nullptr) {
        cleanupXmlDoc(doc);
        *error = "GPX document not parsed successfully.";
        return false;
    }
    return parse(doc, fixes, error);
}
