/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2018 Alberto Griggio <alberto.griggio@gmail.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "rawimagesource.h"
#include "rtthumbnail.h"
#include "curves.h"
#include "color.h"
#include "rt_math.h"
#include "iccstore.h"
#include "../rtgui/mydiagonalcurve.h"
#include "improcfun.h"
#include "StopWatch.h"
#include <iostream>
#include <iomanip>


namespace rtengine {

extern const Settings *settings;

namespace {

std::vector<int> getCdf(const IImage8 &img)
{
    std::vector<int> ret(256);
    for (int y = 0; y < img.getHeight(); ++y) {
        for (int x = 0; x < img.getWidth(); ++x) {
            int lum = LIM(0, int(Color::rgbLuminance(float(img.r(y, x)), float(img.g(y, x)), float(img.b(y, x)))), 255);
            ++ret[lum];
        }
    }

    int sum = 0;
    for (size_t i = 0; i < ret.size(); ++i) {
        sum += ret[i];
        ret[i] = sum;
    }
    
    return ret;
}


int findMatch(int val, const std::vector<int> &cdf, int j)
{
    if (cdf[j] <= val) {
        for (; j < int(cdf.size()); ++j) {
            if (cdf[j] == val) {
                return j;
            } else if (cdf[j] > val) {
                return (cdf[j] - val <= val - cdf[j-1] ? j : j-1);
            }
        }
        return 255;
    } else {
        for (; j >= 0; --j) {
            if (cdf[j] == val) {
                return j;
            } else if (cdf[j] < val) {
                return (val - cdf[j] <= cdf[j+1] - val ? j : j+1);
            }
        }
        return 0;
    }
}


void mappingToCurve(const std::vector<int> &mapping, std::vector<double> &curve)
{
    curve.clear();
    
    const int npoints = 8;
    int idx = 1;
    for (; idx < int(mapping.size()); ++idx) {
        if (mapping[idx] >= idx) {
            break;
        }
    }
    int step = max(int(mapping.size())/npoints, 1);

    auto coord = [](int v) -> double { return double(v)/255.0; };
    auto doit =
        [&](int start, int stop, int step, bool addstart) -> void
        {
            int prev = start;
            if (addstart) {
                curve.push_back(coord(start));
                curve.push_back(coord(mapping[start]));
            }
            for (int i = start; i < stop; ++i) {
                int v = mapping[i];
                bool change = i > 0 && v != mapping[i-1];
                int diff = i - prev;
                if ((change && std::abs(diff - step) <= 1) || diff > step * 2) {
                    curve.push_back(coord(i));
                    curve.push_back(coord(v));
                    prev = i;
                }
            }
        };
    doit(0, idx, idx > step ? step : idx / 2, true);
    doit(idx, int(mapping.size()), step, idx - step > step / 2);
    if (curve.size() > 2 && (1 - curve[curve.size()-2] <= step / (256.0 * 3))) {
        curve.pop_back();
        curve.pop_back();
    }
    // remove the plateau at the end (if any)
    size_t j = curve.size() / 2;
    for (; j > 1; --j) {
        if (std::abs(curve[2*(j-1)-1] - curve.back()) > 0.01) {
            break;
        }
    }
    curve.resize(2*j);
        
    curve.push_back(1.0);
    curve.push_back(1.0);
        
    if (curve.size() < 4) {
        curve = { DCT_Linear }; // not enough points, fall back to linear
    } else {
        curve.insert(curve.begin(), DCT_Spline);
    }
}

} // namespace


void RawImageSource::getAutoMatchedToneCurve(std::vector<double> &outCurve)
{
    BENCHFUN
        
    if (settings->verbose) {
        std::cout << "performing histogram matching for " << getFileName() << " on the embedded thumbnail" << std::endl;
    }

    if (!histMatchingCache.empty()) {
        if (settings->verbose) {
            std::cout << "tone curve found in cache" << std::endl;
            outCurve = histMatchingCache;
            return;
        }
    }

    outCurve = { DCT_Linear };

    int fw, fh;
    getFullSize(fw, fh, TR_NONE);
    int skip = 10;

    if (settings->verbose) {
        std::cout << "histogram matching: full raw image size is " << fw << "x" << fh << std::endl;
    }

    ProcParams neutral;
    neutral.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::FAST);
    neutral.raw.xtranssensor.method = RAWParams::XTransSensor::getMethodString(RAWParams::XTransSensor::Method::FAST);
    
    std::unique_ptr<IImage8> source;
    {
        RawMetaDataLocation rml;
        eSensorType sensor_type;
        int w, h;
        std::unique_ptr<Thumbnail> thumb(Thumbnail::loadQuickFromRaw(getFileName(), rml, sensor_type, w, h, 1, false, true));
        if (!thumb) {
            if (settings->verbose) {
                std::cout << "histogram matching: no thumbnail found, generating a neutral curve" << std::endl;
            }
            histMatchingCache = outCurve;
            return;
        }
        source.reset(thumb->quickProcessImage(neutral, fh / skip, TI_Nearest));

        if (settings->verbose) {
            std::cout << "histogram matching: extracted embedded thumbnail" << std::endl;
        }
    }
    
    std::unique_ptr<IImage8> target;
    {
        RawMetaDataLocation rml;
        eSensorType sensor_type;
        double scale;
        int w = fw / skip, h = fh / skip;
        std::unique_ptr<Thumbnail> thumb(Thumbnail::loadFromRaw(getFileName(), rml, sensor_type, w, h, 1, false, false));
        target.reset(thumb->processImage(neutral, sensor_type, fh / skip, TI_Nearest, getMetaData(), scale, false));

        int sw = source->getWidth(), sh = source->getHeight();
        int tw = target->getWidth(), th = target->getHeight();
        float thumb_ratio = float(std::max(sw, sh)) / float(std::min(sw, sh));
        float target_ratio = float(std::max(tw, th)) / float(std::min(tw, th));
        int cx = 0, cy = 0;
        if (std::abs(thumb_ratio - target_ratio) > 0.01) {
            if (thumb_ratio > target_ratio) {
                // crop the height
                int ch = th - (tw * float(sh) / float(sw));
                cy += ch / 2;
                th -= ch;
            } else {
                // crop the width
                int cw = tw - (th * float(sw) / float(sh));
                cx += cw / 2;
                tw -= cw;
            }
            if (settings->verbose) {
                std::cout << "histogram matching: cropping target to get an aspect ratio of " << std::fixed << std::setprecision(2) << thumb_ratio << ":1, new size is " << tw << "x" << th << std::endl;
            }

            Image8 *tmp = new Image8(tw, th);
#ifdef _OPENMP
            #pragma omp parallel for
#endif
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    tmp->r(y, x) = target->r(y+cy, x+cx);
                    tmp->g(y, x) = target->g(y+cy, x+cx);
                    tmp->b(y, x) = target->b(y+cy, x+cx);
                }
            }
            target.reset(tmp);
        }

        if (settings->verbose) {
            std::cout << "histogram matching: generated neutral rendering" << std::endl;
        }
    }
    if (target->getWidth() != source->getWidth() || target->getHeight() != source->getHeight()) {
        Image8 *tmp = new Image8(source->getWidth(), source->getHeight());
        target->resizeImgTo(source->getWidth(), source->getHeight(), TI_Nearest, tmp);
        target.reset(tmp);
    }
    std::vector<int> scdf = getCdf(*source);
    std::vector<int> tcdf = getCdf(*target);

    std::vector<int> mapping;
    int j = 0;
    for (size_t i = 0; i < tcdf.size(); ++i) {
        j = findMatch(tcdf[i], scdf, j);
        mapping.push_back(j);
    }

    mappingToCurve(mapping, outCurve);

    if (settings->verbose) {
        std::cout << "histogram matching: generated curve with " << outCurve.size()/2 << " control points" << std::endl;
    }

    histMatchingCache = outCurve;
}

} // namespace rtengine
