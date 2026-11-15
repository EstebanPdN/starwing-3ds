#pragma once
#include <algorithm>
#include <cstdlib>

namespace starfox::render {
// MDRAWC's major-axis line, including both endpoints and its strict
// underflow tie rule. Adjacent squares in a row/column have the same paint
// at every screen coordinate, so their union is one rectangle. Keeping
// that union avoids recording a 96-byte command for every line pixel.
template<class Emit>
void source_line_runs(int x0, int y0, int x1, int y1, int thickness, Emit emit) {
    const int dx=std::abs(x1-x0),dy=std::abs(y1-y0);
    const int sx=x0<x1?1:-1,sy=y0<y1?1:-1;
    const int offset=(thickness-1)/2;
    if (dx>=dy) {
        int error=dx>>1,first=x0;
        for (int remaining=dx+1;remaining;--remaining) {
            error-=dy;
            if (error<0 || remaining==1) {
                emit(std::min(first,x0)-offset,y0-offset,
                    std::max(first,x0)-offset+thickness,y0-offset+thickness);
                first=x0+sx;
            }
            if (error<0) {error+=dx;y0+=sy;}
            x0+=sx;
        }
    } else {
        int error=dy>>1,first=y0;
        for (int remaining=dy+1;remaining;--remaining) {
            error-=dx;
            if (error<0 || remaining==1) {
                emit(x0-offset,std::min(first,y0)-offset,
                    x0-offset+thickness,std::max(first,y0)-offset+thickness);
                first=y0+sy;
            }
            if (error<0) {error+=dy;x0+=sx;}
            y0+=sy;
        }
    }
}
}
