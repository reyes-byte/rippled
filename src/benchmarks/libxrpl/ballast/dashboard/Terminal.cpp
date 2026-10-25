#include <sys/ioctl.h>

#include <Dashboard.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <stdexcept>

namespace ballast::dashboard {
namespace {

std::sig_atomic_t volatile stopped = 0;
void
stop(int)
{
    stopped = 1;
}

// RAII restores terminal settings on normal exit and C++ exceptions. Signal
// handlers only set a flag; all terminal/output work stays in the main loop.
class Terminal
{
    termios original_{};
    using Handler = void (*)(int);
    Handler previousInt_;
    Handler previousTerm_;
    Handler previousHup_;

public:
    Terminal()
    {
        if (tcgetattr(STDIN_FILENO, &original_) != 0)
            throw std::runtime_error("Cannot read terminal settings; use --plain");
        auto settings = original_;
        settings.c_lflag &= ~(ICANON | ECHO | ISIG);
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0)
            throw std::runtime_error("Cannot set terminal mode; use --plain");
        stopped = 0;
        previousInt_ = std::signal(SIGINT, stop);
        previousTerm_ = std::signal(SIGTERM, stop);
        previousHup_ = std::signal(SIGHUP, stop);
        std::cout << "\033[?1049h\033[?25l" << std::flush;
    }
    ~Terminal()
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_);
        std::signal(SIGINT, previousInt_);
        std::signal(SIGTERM, previousTerm_);
        std::signal(SIGHUP, previousHup_);
        std::cout << "\033[0m\033[?25h\033[?1049l" << std::flush;
    }
    Terminal(Terminal const&) = delete;
    Terminal&
    operator=(Terminal const&) = delete;
};

char
readKey(int timeout)
{
    pollfd input{STDIN_FILENO, POLLIN, 0};
    int const status = poll(&input, 1, timeout);
    if (status < 0 && errno != EINTR)
        throw std::runtime_error("Terminal poll failed");
    if (status <= 0)
        return 0;
    if (input.revents & (POLLHUP | POLLERR | POLLNVAL))
        return 'q';
    char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) != 1)
        return 0;
    return ch;
}

}  // namespace

int
interactive(Snapshot const& snapshot, State& state)
{
    Terminal terminal;
    std::size_t previousWidth = 0, previousHeight = 0;
    bool redraw = true;
    while (!stopped)
    {
        winsize size{};
        auto const valid = ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0;
        std::size_t const width = valid && size.ws_col ? size.ws_col : 80;
        std::size_t const height = valid && size.ws_row ? size.ws_row : 24;
        if (redraw || width != previousWidth || height != previousHeight)
        {
            std::cout << "\033[H\033[2J";
            auto const lines = render(snapshot, state, width, height);
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                auto const& line = lines[i];
                if (i)
                    std::cout << "\r\n";
                if (!line.empty() && line.front() == '>')
                    std::cout << "\033[1;36m";
                else if (line.starts_with("BALLAST"))
                    std::cout << "\033[1;36m";
                else if (line.starts_with("Coverage:"))
                    std::cout << "\033[33m";
                std::cout << line << "\033[0m";
            }
            std::cout << std::flush;
            previousWidth = width;
            previousHeight = height;
            redraw = false;
        }
        char key = readKey(200);
        if (key == '\033' && readKey(30) == '[')
        {
            char const arrow = readKey(30);
            key = arrow == 'A' ? 'k' : (arrow == 'B' ? 'j' : 0);
        }
        if (key)
        {
            if (!state.handle(key, snapshot.views.size()))
                break;
            redraw = true;
        }
    }
    return 0;
}

}  // namespace ballast::dashboard
