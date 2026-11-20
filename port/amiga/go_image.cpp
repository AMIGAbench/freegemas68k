/* GoSDL::Image for AmigaOS: .gfx textures (RGB565 + A8) made by
 * tools/convert_assets.py from the original PNGs. */

#include "go_image.h"
#include "go_window.h"
#include "amiga_internal.h"
#include "Util.h"

static std::string gfx_path(const std::string &path)
{
    size_t dot = path.rfind('.');
    if (dot != std::string::npos && path.compare(dot, std::string::npos, ".png") == 0)
        return path.substr(0, dot) + ".gfx";
    return path;
}

GoSDL::Image::Image() : mParentWindow(NULL), mWidth(0), mHeight(0) { }

GoSDL::Image::Image(GoSDL::Window *parentWindow, string path)
    : mParentWindow(parentWindow), mWidth(0), mHeight(0), mPath(getBasePath() + path)
{
    loadTexture();
}

GoSDL::Image::Image(const Image &other)
    : mTexture(other.mTexture), mParentWindow(other.mParentWindow),
      mWidth(other.mWidth), mHeight(other.mHeight), mPath(other.mPath) { }

GoSDL::Image::Image(Image &&other)
    : mTexture(other.mTexture), mParentWindow(other.mParentWindow),
      mWidth(other.mWidth), mHeight(other.mHeight), mPath(other.mPath)
{
    other.mTexture.reset();
}

GoSDL::Image::~Image()
{
    mParentWindow = NULL;
}

GoSDL::Image &GoSDL::Image::operator=(GoSDL::Image &&other)
{
    if (this != &other) {
        mParentWindow = other.mParentWindow;
        mPath = other.mPath;
        mWidth = other.mWidth;
        mHeight = other.mHeight;
        mTexture = other.mTexture;
    }
    return *this;
}

void GoSDL::Image::setWindow(GoSDL::Window *parentWindow)
{
    mParentWindow = parentWindow;
}

void GoSDL::Image::setPath(std::string path)
{
    mPath = getBasePath() + path;
    loadTexture();
}

bool GoSDL::Image::setWindowAndPath(GoSDL::Window *parentWindow, std::string path)
{
    mParentWindow = parentWindow;
    mPath = getBasePath() + path;
    return loadTexture();
}

bool GoSDL::Image::loadTexture()
{
    /* shared, preloaded texture (never a second deleter on the same texture) */
    std::shared_ptr<SDL_Texture> t = fg_gfx_get(gfx_path(mPath));
    if (!t) return false;
    mTexture = t;
    mWidth = t->w;
    mHeight = t->h;
    return true;
}

void GoSDL::Image::setTexture(SDL_Texture *texture)
{
    mTexture.reset(texture, GoSDL::Image::SDL_Texture_Deleter());
    mWidth = texture ? texture->w : 0;
    mHeight = texture ? texture->h : 0;
}

int GoSDL::Image::getWidth()  { return mWidth; }
int GoSDL::Image::getHeight() { return mHeight; }

bool GoSDL::Image::draw(int x, int y, int z, double factorX, double factorY, float angle, Uint8 alpha, SDL_Color color)
{
    if (mParentWindow == NULL || mTexture == NULL) return false;

    SDL_FRect destRect;
    destRect.w = mWidth * factorX;
    destRect.h = mHeight * factorY;
    destRect.x = x;
    destRect.y = y;
    mParentWindow->enqueueDraw(mTexture.get(), destRect, angle, z, alpha, color);
    return true;
}
