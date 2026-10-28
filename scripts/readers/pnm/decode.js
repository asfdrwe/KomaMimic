// SPDX-License-Identifier: GPL-3.0-only
// Concatenated binary PPM (P6), RGB, maxval 1..65535. Whitespace/comments in headers.
function frames(input, context) {
    const bytes=new Uint8Array(input); let pos=0, result=[];
    function token() {
        while(pos<bytes.length) {
            if(bytes[pos]===35) {while(pos<bytes.length && bytes[pos]!==10)++pos;}
            else if([9,10,11,12,13,32].includes(bytes[pos]))++pos; else break;
        }
        let s='';while(pos<bytes.length && ![9,10,13,32,35].includes(bytes[pos]))s+=String.fromCharCode(bytes[pos++]);
        return s;
    }
    while(pos<bytes.length) {
        const magic=token();if(!magic)break;
        if(magic!=='P6')throw 'Only binary RGB PPM (P6) is supported';
        const w=Number(token()), h=Number(token()), max=Number(token());
        if(!Number.isSafeInteger(w)||!Number.isSafeInteger(h)||w<1||h<1||w*h>context.maxPixels||!Number.isInteger(max)||max<1||max>65535)throw 'Unsupported PPM dimensions/maxval';
        if(![9,10,11,12,13,32].includes(bytes[pos]))throw 'Missing raster separator';
        // Exactly one separator; CRLF is a single line ending. Pixel whitespace is data.
        if(bytes[pos++]===13 && bytes[pos]===10)++pos;
        const stride=max>255 ? 2 : 1;
        if(pos+w*h*3*stride>bytes.length)throw 'Truncated PPM raster';
        result.push({width:w,height:h,offset:pos,max,stride});pos+=w*h*3*stride;
        if(result.length>context.maxPages)throw 'Too many pages';
    }
    if(!result.length)throw 'Empty PPM';return result;
}
function inspect(input,context) {return {pages:frames(input,context).length};}
function decode(input,context) {
    const f=frames(input,context)[context.pageIndex];if(!f)throw 'Page out of range';
    const src=new Uint8Array(input), dst=new Uint8Array(f.width*f.height*4);
    let at=f.offset;
    for(let i=0;i<dst.length;i+=4) {
        for(let c=0;c<3;++c) {
            const value=f.stride===2 ? src[at]*256+src[at+1] : src[at];at+=f.stride;
            if(value>f.max)throw 'PPM component exceeds maxval';
            dst[i+c]=Math.round(value*255/f.max);
        }
        dst[i+3]=255;
    }
    return {width:f.width,height:f.height,pixels:dst.buffer};
}
