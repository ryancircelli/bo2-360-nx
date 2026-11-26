// macOS Vision OCR; outputs recognized text and normalized image coordinates.
ObjC.import('Foundation');
ObjC.import('Vision');
function run(argv) {
    const request = $.VNRecognizeTextRequest.alloc.init;
    request.recognitionLevel = $.VNRequestTextRecognitionLevelAccurate;
    request.usesLanguageCorrection = false;
    const handler = $.VNImageRequestHandler.alloc.initWithURLOptions(
        $.NSURL.fileURLWithPath(argv[0]), $.NSDictionary.alloc.init);
    if (!handler.performRequestsError($.NSArray.arrayWithObject(request), null)) throw Error('Vision OCR failed');
    const results = request.results;
    const words = [];
    for (let i = 0; i < results.count; i++) {
        const item = results.objectAtIndex(i);
        const box = item.boundingBox;
        words.push({text: item.topCandidates(1).objectAtIndex(0).string.js,
                    x: box.origin.x, y: 1 - box.origin.y - box.size.height,
                    w: box.size.width, h: box.size.height});
    }
    return JSON.stringify(words);
}
