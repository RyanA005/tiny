this is a very tiny web server written by Ryan Alport for demo use

![default page](www/assets/default.png)

while this server has many features and performs incredibly well in select test cases, it is not proven for production use

compile the project with Make, this project targets 64-bit linux machines and makes no cross platform claims

a server executable will be generated as well as a file called www/defaults.tiny, this contains the compiled constants the server uses for its config

config supports a worker thread count, listening backlogs, queue sizes, connection allocated buffer sizes, etc. recompile to update the executable

the project is built as a learning experiment as well as a demo on optimizing low level programs

the accompanying presentation can be found here: https://iheartcomputer.club/projects/web-server/

more information on the architecture and design choices can be found there

reach out to me at ralport2005@gmail.com with any questions at all!
