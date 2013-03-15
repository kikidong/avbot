
#include <boost/locale.hpp>

#include <boost/bind.hpp>
#include <boost/regex.hpp>
#include <boost/foreach.hpp>

#include "boost/base64.hpp"
#include "pop3.hpp"

static inline std::string ansi_utf8(std::string const &source, const std::string &characters = "GB18030")
{
	return boost::locale::conv::between(source, "UTF-8", characters);
}

template<typename strIerator, typename Char>
static std::string endmark(strIerator & s,strIerator e, Char delim)
{
	std::string str;
	while( s !=  e && *s != delim){
		str.append(1,*s);
		s++;
	}
	return str;
}

static std::string base64inlinedecode(std::string str)
{
	std::string result;
	std::string::iterator p = str.begin();
	while(p!=str.end())
	{
		if( *p == '=' && *(p+1)=='?') // 进入　base64 模式
		{
			p+=2;
			std::string charset = endmark(p,str.end(),'?');
			p+=3; // skip "?B?"
			std::string base64encoded = endmark(p,str.end(),'?');
			result.append(ansi_utf8(boost::base64_decode(base64encoded), charset));
			p+=2; // skip ?=
		}else{
			result.append(1,*p);
			p++;
		}
	}
	return result;
}

static std::string find_charset(std::string contenttype)
{
	boost::cmatch what;
	//charset="xxx"
	std::size_t p =  contenttype.find("charset=");
	if( p != contenttype.npos )
	{
		std::string charset = contenttype.substr(p).c_str();
		boost::regex ex("charset=?(.*)?");
		if(boost::regex_match(charset.c_str(), what, ex))
		{
			std::string charset = what[1];
			boost::trim_if(charset,boost::is_any_of(" \""));
			return charset;
		}
	}
	return "UTF-8"; // default to utf8
}

static void decode_mail(boost::asio::io_service & io_service, mailcontent thismail)
{
 	std::cout << "邮件内容begin" << std::endl;

	thismail.from = base64inlinedecode( thismail.from );
	thismail.to = base64inlinedecode( thismail.to );
	std::cout << "发件人:";
	std::cout << thismail.from ;
	std::cout << std::endl;

	typedef std::pair<std::string,std::string> mc;
	
	BOOST_FOREACH(mc &v, thismail.content)
	{
		// 从 v.first aka contenttype 找到编码.
		std::string charset = find_charset(v.first);
		std::string contentcecoded = boost::base64_decode(v.second);
		std::string content = ansi_utf8(contentcecoded, charset);
		std::cout << content << std::endl;
	}
 	std::cout << "邮件内容end" << std::endl;
}

void pop3::process_mail ( std::istream& mail )
{
	int state = 0;
	mailcontent thismail;
	std::string	contenttype;
	std::string content;
	std::string boundary;

	std::string line;
	std::getline ( mail, line );
	boost::trim_right(line);

	// 以行为单位解析
	while ( !mail.eof() ) {
		boost::trim_right(line);
		std::string key, val;
		std::vector<std::string> kv;
		std::stringstream	linestream ( line );

		switch ( state ) {
			case 0: // 忽略起始空白.
				if ( line != "" ) {
					state = 1;
				}else
					break;
			case 1: // 解析　　XXX:XXX　文件头选项对.
			case 5: // MIME 子头，以 ------=_NextPart_51407E7E_09141558_64FF918C 的形式起始.
				if (line == "" ){ // 空行终止文件头.
					if( state == 1 )
						if(boundary.empty()){ //进入非MIME格式的邮件正文处理.
							state = 3;
						}else{
							state = 4; //正文为　MIME , 进入　MIME 模式子头处理.
						}
					else {
						state = 6; // MIME 模式的子body处理
					}
					break;
				}
				if( state == 5 && line == std::string("--") + boundary + "--"){
					state = 7 ; //MIME 结束.
				}
				//　进入解析.
				boost::split ( kv, line, boost::algorithm::is_any_of ( ":" ) );
				key = kv[0];
				val = kv[1];
				boost::to_lower ( key );
				boost::to_lower ( val );

				if ( key == "from" ) {
					thismail.from = kv[1];
				} else if ( key == "to" ) {
					thismail.to = kv[1];
				} else if ( key == "subject" ) {
					thismail.subject = kv[1];
				} else if ( key == "content-type" ) {
					// 检查是不是　MIME　类型的.
					if ( boost::trim_copy ( val ) == "multipart/alternative;" ) {
						//是　MIME 类型的.
						state = 2;
						//进入　boundary 设定.
					} else {
						// 记录 content-type
						contenttype = val;
						if( state != 1){
							state = 7;
						}else{
							state = 1;
						}
					}
				}

				break;
			case 2:
			{
				using namespace boost;
				cmatch what;
				if(regex_match(line.c_str(),what,regex("\\tboundary=?\"(.*)?\"")))
				{
					boundary = what[1];
				}
			}
			state = 1;
			break;

			case 3: // 是　multipart 的么?
			case 6:
				if(state==3 && line == "."){
					state = 0;
					thismail.content.push_back(std::make_pair(contenttype,content));
					content.clear();
				}else if( state == 6 && line == std::string("--")+ boundary ){
					state = 5;
					thismail.content.push_back(std::make_pair(contenttype,content));
					content.clear();
				}else if ( state == 6 && line == std::string("--") + boundary + "--")
				{
					state = 8;
					thismail.content.push_back(std::make_pair(contenttype,content));
					content.clear();
				}else{
					//读取 content.
					content += line;
				}
				break;
			case 4: // 如果是　4 , 则是进入 MIME 的子头处理.
				// 查找 boundary
				if(line == std::string("--")+ boundary )
				{
					state = 5;
				}
				break;
			case 7: // charset = ??
				contenttype += line;
				state = 5;
				break;
			case 8:
				break;
		}
		line.clear();
		std::getline ( mail, line );
	}
	// 处理　base64 编码的邮件内容.
	io_service.post(boost::bind(decode_mail, boost::ref(io_service), thismail));
}


