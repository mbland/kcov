#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <string>
#include <unordered_map>
#include <mutex>

#include <system-mode/registration.hh>
#include <system-mode/file-data.hh>
#include <engine.hh>
#include <utils.hh>
#include <configuration.hh>

#include "engines/system-mode-file-format.hh"

#include <unordered_map>

using namespace kcov;

static std::mutex outputFileMutex;
static std::unordered_map<uint32_t, kcov_system_mode::system_mode_memory *> outputFiles;

class ProcessHandler : public IEngine::IEventListener
{
public:
	ProcessHandler(const std::string destinationDir, const std::string &filename, pid_t pid,
			kcov_system_mode::system_mode_memory *output) :
		m_filename(filename),
		m_destinationDir(destinationDir),
		m_pid(pid),
		m_engine(NULL),
		m_output(output),
		m_id(0)
	{
		IConfiguration &conf = IConfiguration::getInstance();

		conf.setKey("attach-pid", m_pid);
	}

	virtual ~ProcessHandler()
	{
	}

	bool attach(const SystemModeFile *sysFile)
	{
		IEngineFactory::IEngineCreator &engineCreator = IEngineFactory::getInstance().matchEngine(m_filename);

		m_engine = engineCreator.create();
		if (!m_engine)
		{
			kcov_debug(INFO_MSG, "Can't create engine\n");

			return false;
		}

		if (!m_engine->start(*this, m_filename))
		{
			kcov_debug(INFO_MSG, "Can't start engine for %s:%d\n", m_filename.c_str(), m_pid);

			return false;
		}

		const std::vector<uint64_t> &entries = sysFile->getEntries();
		uint32_t index = 0;

		m_id = sysFile->getId();

		for (std::vector<uint64_t>::const_iterator it = entries.begin();
				it != entries.end();
				++it, ++index)
		{
			m_indexByAddress[*it] = index;
			m_engine->registerBreakpoint(*it);
		}

		return true;
	}

	void run()
	{
		if (!m_output)
		{
			panic("Output not created in run(), check implementation\n");
		}



		while (1)
		{
			bool rv = continueExecution();

			// Exit!
			if (!rv)
			{
				break;
			}
		}
	}

	bool continueExecution()
	{
		return m_engine->continueExecution();
	}

private:
	void onEvent(const IEngine::Event &ev)
	{
		// The others are not very interesting
		if (ev.type == ev_breakpoint)
		{
			IndexMap_t::iterator it = m_indexByAddress.find(ev.addr);

			if (it != m_indexByAddress.end())
			{
				m_output->reportIndex(it->second);
			}
		}
	}


	const std::string m_reportFilename;
	const std::string m_filename;
	const std::string m_destinationDir;

	pid_t m_pid;
	IEngine *m_engine;

	typedef std::unordered_map<uint64_t, uint32_t> IndexMap_t;
	IndexMap_t m_indexByAddress;

	kcov_system_mode::system_mode_memory *m_output;
	uint32_t m_id;
};

static SystemModeFile *getSystemModeFileFromFilename(const std::string &m_filename)
{
	size_t sz;
	void *p;

	p = read_file(&sz, "%s", m_filename.c_str());
	if (!p)
	{
		kcov_debug(INFO_MSG, "Can't read %s\n", m_filename.c_str());

		return NULL;
	}

	SystemModeFile *sysFile = SystemModeFile::fromProcessedFile(p, sz);
	free(p);

	if (!sysFile)
	{
		kcov_debug(INFO_MSG, "Can't unmarshal %s\n", m_filename.c_str());

		return NULL;
	}

	return sysFile;
}

static void processOne(const std::string &destinationDir, const std::string &filename, pid_t pid)
{
	Semaphore sem(0);

	SystemModeFile *sysFile = getSystemModeFileFromFilename(filename);
	if (!sysFile)
	{
		return;
	}

	// Create output memory representation (mmap:ed)
	outputFileMutex.lock();
	kcov_system_mode::system_mode_memory *output = outputFiles[sysFile->getId()];
	if (!output)
	{
		output = new kcov_system_mode::system_mode_memory(sysFile->getFilename(), sysFile->getOptions(), sysFile->getEntries().size());
		outputFiles[sysFile->getId()] = output;
	}
	outputFileMutex.unlock();


	// Create a child and process everything there
	pid_t child = fork();
	if (child < 0)
	{
		kcov_debug(INFO_MSG, "Can't fork?");

		return;
	}
	else if (child == 0)
	{
		// Child
		ProcessHandler handler(destinationDir, filename, pid, output);

		// Parse and run!
		bool ok = handler.attach(sysFile);
		if (ok)
		{
			// Awake the process in open() for the readback FIFO
			handler.continueExecution();
			sem.notify();
			handler.run();
		}
		else
		{
			sem.notify();
		}

		exit(0);
	}
	else
	{
		// Parent, wait until attached
		sem.wait();
	}

	delete sysFile;
}

static bool doExit = false;
static void onExitSignal(int sig)
{
	// Forward the signal to the traced program
	doExit = true;
}

static void *outputThread(void *p)
{
	std::string *in = (std::string *)p;
	std::string destinationDir = *in;

	(void)mkdir(destinationDir.c_str(), 0755);

	while (1)
	{
		if (doExit)
		{
			break;
		}

		// Write data every two seconds
		sleep(2);

		// Copy to a list to process
		std::vector<std::pair<uint32_t, kcov_system_mode::system_mode_memory *>> toProcess;
		outputFileMutex.lock();
		for (std::unordered_map<uint32_t, kcov_system_mode::system_mode_memory *>::iterator it = outputFiles.begin();
				it != outputFiles.end();
				++it)
		{
			toProcess.push_back(std::pair<uint32_t, kcov_system_mode::system_mode_memory *>(it->first, it->second));
		}
		outputFileMutex.unlock();

		for (std::vector<std::pair<uint32_t, kcov_system_mode::system_mode_memory *>>::iterator it = toProcess.begin();
				it != toProcess.end();
				++it)
		{
			kcov_system_mode::system_mode_memory *cur = it->second;

			// Not changed, don't write
			if (!cur->isDirty())
			{
				continue;
			}
			// Mark as clean and write the data
			cur->markClean();


			size_t size;
			struct kcov_system_mode::system_mode_file *dst = kcov_system_mode::memoryToFile(*cur, size);

			if (!dst)
			{
				error("system-mode-daemon: Can't marshal data\n");
				continue;
			}

			std::string out = fmt("%s/%08lx", destinationDir.c_str(), (long)it->first);
			FILE *fp = fopen(out.c_str(), "w");

			if (fp)
			{
				fwrite(dst, 1, size, fp);
				fclose(fp);
			}
			else
			{
				fprintf(stderr, "kcov-binary-lib: Can't write outfile\n");
			}

			free(dst);
		}
	}

	return NULL;
}

int main(int argc, const char *argv[])
{
	char buf[4096];
	const char *path = getenv("KCOV_SYSTEM_DESTINATION_DIR");

	std::string destinationDir = "/tmp/kcov-data";

	if (path)
		destinationDir = path;

	(void)::mkdir(destinationDir.c_str(), 0755);

	const char *pipePath = "/tmp/kcov-system.pipe";
	(void)::mkfifo(pipePath, 0644);

	int fd = ::open(pipePath, O_RDONLY);
	if (fd < 0)
	{
		fprintf(stderr, "Can't open fifo %s\n", pipePath);
		exit(1);
	}

	// Catch Cltr-C and kill
	signal(SIGINT, onExitSignal);
	signal(SIGTERM, onExitSignal);

	pthread_t thread;
	pthread_create(&thread, NULL, outputThread, (void *)&destinationDir);

	while (1)
	{
		if (doExit)
		{
			break;
		}

		int r = ::read(fd, buf, sizeof(buf));

		if (r <= 0)
		{
			continue;
		}

		if (r < (int)sizeof(struct new_process_entry))
		{
			continue;
		}

		uint16_t pid;
		std::string entryFilename;

		struct new_process_entry *p = (struct new_process_entry *)buf;

		if (!parseProcessEntry(p, pid, entryFilename))
		{
			continue;
		}

		processOne(destinationDir, entryFilename, pid);

		// Write a character to let the process loose again
		write_file(buf, 1, "%s/%d", destinationDir.c_str(), pid);
	}

	// Wait for the reporter thread
	void *rv;
	pthread_join(thread, &rv);
}
