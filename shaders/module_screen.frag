// Paints the map with the render module's frame (RenderModuleView): each
// pixel takes the module's colour at the same place on screen, so the map
// looks as the module drew it while still hiding what lies behind it. The
// module's image may reach past the view (moduleImageOrigin is where the
// view's top left falls in it).
void MAIN()
{
    vec2 pixel = FRAGCOORD.xy;
    // The frame's rows run top down.
    if (FRAMEBUFFER_Y_UP > 0.0)
        pixel.y = viewSize.y - pixel.y;
    vec2 coordinate = (pixel + moduleImageOrigin) / max(moduleImageSize, vec2(1.0));
    FRAGCOLOR = vec4(texture(moduleImage, coordinate).rgb, 1.0);
}
