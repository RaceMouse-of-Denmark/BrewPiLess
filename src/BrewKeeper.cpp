#include <FS.h>
#include <ArduinoJson.h>

#include <time.h>
#include "BrewPiProxy.h"
#include "BrewKeeper.h"
#include "mystrlib.h"

#if EnableGravitySchedule

#define BrewStatusFile "/brewing.s"
#define CurrentProfileVersion 2

#endif

#define OUT_OF_RANGE(a,b,c) ((((a) - (b)) > (c)) || (((a) - (b)) < -(c)))
#define IS_INVALID_CONTROL_TEMP(t) ((t)< -99.0)
#define INVALID_CONTROL_TEMP -100.0

void BrewKeeper::keep(time_t now)
{

	if((now - _lastSetTemp) < MINIMUM_TEMPERATURE_SETTING_PERIOD) return;
	_lastSetTemp= now;

	char unit, mode;
	float beerSet,fridgeSet;
	brewPi.getControlParameter(&unit,&mode,&beerSet,&fridgeSet);

	// run in loop()
	if (mode != 'p') return;

	if((_profile.loaded()!=true) && (_filename!=NULL)){
		//DBG_PRINTF("load profile.\n");
		_profile.load(_filename);
	}
	if(! _profile.loaded()) return;

	// check unit
	_profile.setUnit(unit);

	#if EnableGravitySchedule
	float temp=_profile.tempByTimeGravity(now,_lastGravity);
	#else
	float temp=_profile.tempByTime(now);
	#endif

	if(IS_INVALID_CONTROL_TEMP(temp)) return;
	if(OUT_OF_RANGE(temp,beerSet,MINIMUM_TEMPERATURE_STEP)){
		// set temp
		//_write("j{beerSet:" + temp + "}");
		char buff[36];
		strcpy(buff,"j{beerSet:");
		int len=strlen(buff);
		len+=sprintFloat(buff+len,temp,2);
		strcpy(buff+len,"}");

		//BPSerial.print(buff);
		_write(buff);

	}
}

//**********************************************************************************
//class BrewProfile
//**********************************************************************************
#define F2C(d) (((d)-32)/1.8)
#define C2F(d) (((d)*1.8)+32)


#if EnableGravitySchedule

void BrewProfile::_tempConvert(void)
{
	for(int i=0;i< _numberOfSteps;i++){
		_steps[i].temp = (_unit == 'C')? F2C(_steps[i].temp):C2F(_steps[i].temp);
	}
}

void BrewProfile::setUnit(char unit)
{
	if(_unit == unit) return;
	_unit = unit;

	if(!_profileLoaded) return;
	_tempConvert();
}
time_t tm_to_timet(struct tm *tm_time);


bool BrewProfile::load(String filename)
{
	time_t savedStart=0;
	if(!_statusLoaded){
		_statusLoaded = true;
		savedStart=_loadBrewingStatus(); // status need to load before profile
	}
	// the profile might be "reloaded" after user update
	// or the first time loaded. in that case. the status is empty
    if(_loadProfile(filename)){
		if(savedStart){
			if((savedStart != _startDay) ||
				(_timeEnterCurrentStep < _startDay)){
				// start day is later. that meas a new start
				_currentStep=0;
				_timeEnterCurrentStep=0;
				DBG_PRINTF("New profile:st %ld  _timeEnterCurrentStep:%ld!\n",_startDay,_timeEnterCurrentStep);
			}else{
				if(_currentStep >= _numberOfSteps){
					DBG_PRINTF("error step: %d >= %d\n",_currentStep,_numberOfSteps);
				}else{
					_currentStepDuration =(time_t)(_steps[_currentStep].days * 86400);
				}
			}
		}
        return true;
    }else
        return false;
}

bool BrewProfile::_loadProfile(String filename)
{
	//DBG_PRINTF("BrewProfile::load\n");

	if(!SPIFFS.exists(filename)){
		//DBG_PRINTF("file:%s not exist\n",filename.c_str());
		return false;
	}
	File pf=SPIFFS.open(filename,"r");
	if(!pf){
		DBG_PRINTF("profile open failed\n");
		return false;
	}
	char profileBuffer[MAX_PROFILE_LEN];
	size_t len=pf.readBytes(profileBuffer,MAX_PROFILE_LEN);
	pf.close();
	DBG_PRINTF("Profile length:%d\n",len);
	profileBuffer[len]='\0';

	const int PROFILE_JSON_BUFFER_SIZE = JSON_ARRAY_SIZE(15) + 7*JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + 3*JSON_OBJECT_SIZE(4) + 5*JSON_OBJECT_SIZE(6);
	DynamicJsonBuffer jsonBuffer(PROFILE_JSON_BUFFER_SIZE);
	JsonObject& root = jsonBuffer.parseObject(profileBuffer);

	if(!root.success()){
		DBG_PRINTF("JSON parsing failed json size:%d\n",PROFILE_JSON_BUFFER_SIZE);
		return false;
	}
	if(!root.containsKey("s")
		|| !root.containsKey("u")
		|| !root.containsKey("t")
		|| !root.containsKey("v")){
		DBG_PRINTF("JSON file not include necessary fields\n");
		return false;
	}
	int version=root["v"];
	if( version !=CurrentProfileVersion){
		DBG_PRINTF("profile version:%d\n",version);
		return false;
	}
	if (!root["t"].is<JsonArray&>()){
		DBG_PRINTF("JSON t is not array\n");
		return false;
	}
	// get starting time
	//ISO time:
	//2016-07-01T05:22:33.351Z
	//01234567890123456789
	tm tmStart;
	char buf[8];
	const char* sdutc=root["s"];

	#define GetValue(d,s,l) strncpy(buf,sdutc+s,l);buf[l]='\0';d=atoi(buf)
	GetValue(tmStart.tm_year,0,4);
	tmStart.tm_year -= 1970; //1900;
	GetValue(tmStart.tm_mon,5,2);
//	tmStart.tm_mon -= 1;
	GetValue(tmStart.tm_mday,8,2);
	GetValue(tmStart.tm_hour,11,2);
	GetValue(tmStart.tm_min,14,2);
	GetValue(tmStart.tm_sec,17,2);

	DBG_PRINTF("%d/%d/%d %d:%d:%d\n",tmStart.tm_year,tmStart.tm_mon,tmStart.tm_mday,
		tmStart.tm_hour,tmStart.tm_min,tmStart.tm_sec);

	//_startDay = mktime(&tmStart);

	_startDay= tm_to_timet(&tmStart);

	JsonArray& schedule = root["t"];
	_numberOfSteps=schedule.size();

	if(!_steps) free(_steps);
	_steps =(ProfileStep*) malloc(sizeof(ProfileStep) * _numberOfSteps);

	int i=0;

	for(int i=0;i< _numberOfSteps ;i++){
		JsonObject&	 entry= schedule[i];
		//{"c":"g","d":6,"t":12,"g":1.026},{"c":"r","d":1}
		const char* constr= entry["c"];
		_steps[i].condition = *constr;
		_steps[i].days = entry["d"];

		DBG_PRINTF("%d ,type:%c time:",i,_steps[i].condition );
		DBG_PRINT(_steps[i].days);

		if(_steps[i].condition != 'r'){
			_steps[i].temp= entry["t"];

			if(entry.containsKey("g")){
    			if(entry["g"].is<const char*>()){
    			    const char* attStr=entry["g"];
    			    float att=atof(attStr);
    			    if( strchr ( attStr, '%' ) > 0){
                        float csg=1.0 + (_OGPoints * (100.0 - att)/100.0);
                        _steps[i].sg = FloatToGravity(csg);
    			    DBG_PRINTF(" att:%s sg:%d",attStr,_steps[i].sg);

                    }else{
        			    float fsg= entry["g"];
	        		    _steps[i].sg = FloatToGravity(fsg);
    	    		    DBG_PRINTF(" sg:%d",_steps[i].sg);
                    }
    			}else{
    			    float fsg= entry["g"];
	    		    _steps[i].sg = FloatToGravity(fsg);
    			    DBG_PRINTF(" sg:%d",_steps[i].sg);
    		    }
			}

			if(entry.containsKey("s")){
    			int st= entry["s"];
	    		_steps[i].stableTime =st;
	    		_steps[i].stablePoint=(entry.containsKey("x"))? entry["x"]:_stableThreshold;

    			DBG_PRINTF(" Stable :%d@%d",_steps[i].stablePoint,_steps[i].stableTime);
			}

			DBG_PRINT(" temp:");
			DBG_PRINT(_steps[i].temp);
		}
		DBG_PRINTF("\n");
	}
	_profileLoaded=true;

	// unit
	const char *punit = root["u"];
	char unit = *punit;
	if(_unit != 'U' && _unit != unit){ // been set by controller
		_unit = unit;
		_tempConvert();
	}else
		_unit = unit;

	DBG_PRINTF("finished, st:%ld, unit:%c, _numberOfSteps:%d\n",_startDay,unit,_numberOfSteps);

	return true;
}

void BrewProfile::setOriginalGravity(float gravity){
    _OGPoints = gravity - 1.0;
    _saveBrewingStatus();
}

#define MAX_BREWING_STATE_LEN 256

void BrewProfile::_saveBrewingStatus(void){
	File pf=SPIFFS.open(BrewStatusFile,"w");
	if(pf){
	    int ogpoints=(int) round(_OGPoints * 1000.0);
		const int STATUS_JSON_BUFFER_SIZE =  2 *JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(3);

		DynamicJsonBuffer jsonBuffer(STATUS_JSON_BUFFER_SIZE);
		JsonObject& root = jsonBuffer.createObject();
		root["s"] = _currentStep;
		root["e"] = _timeEnterCurrentStep;
		root["b"] =_startDay;
		root["og"] = ogpoints;
		root.printTo(pf);
		//pf.printf("%d\n%ld\n%ld\n%d\n",_currentStep,_timeEnterCurrentStep,_startDay,ogpoints);
	}
	pf.close();
}

time_t BrewProfile::_loadBrewingStatus(void){
	
	time_t savedStart =0;

	File pf=SPIFFS.open(BrewStatusFile,"r");
	
	_currentStep=0;
	_timeEnterCurrentStep=0;

	if(pf){

		char buffer[MAX_BREWING_STATE_LEN];
		size_t len=pf.readBytes(buffer,MAX_BREWING_STATE_LEN);
		pf.close();
		buffer[len]='\0';
		DBG_PRINTF("Profile length:%d %s\n",len,buffer);
		const int STATUS_JSON_BUFFER_SIZE =  2 *JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(3);
		DynamicJsonBuffer jsonBuffer(STATUS_JSON_BUFFER_SIZE);
		JsonObject& root = jsonBuffer.parseObject(buffer);

		if(!root.success() 
			|| !root.containsKey("s")
			|| !root.containsKey("e")
			|| !root.containsKey("b")
			|| !root.containsKey("og")	){
			DBG_PRINTF("brew status failed, size:%d\n",STATUS_JSON_BUFFER_SIZE);
			return 0;
		}
		_currentStep=root["s"];
		_timeEnterCurrentStep=root["e"];
		savedStart=root["b"];
		int ogpoints=root["og"];

		DBG_PRINTF("step:%d, since:%ld, begin:%ld, og:%d\n",_currentStep,_timeEnterCurrentStep,savedStart,ogpoints);

        _OGPoints = (float)ogpoints /1000.0;
	}else{
		DBG_PRINTF("%s open failed\n",BrewStatusFile);
		// try to figure out where we were
	}
	pf.close();
	return savedStart;
}

void BrewProfile::_estimateStep(time_t now)
{
	_timeEnterCurrentStep = _startDay;
	_currentStep =0;
/*
	time_t stime=_startDay;
	for(int i=0;i<_numberOfSteps;i++)
	{
		time_t duration=(time_t)(_steps[i].days * 86400);
		time_t next= stime + duration;

		if(stime <= now && now < next ){
			_currentStep = i;
			_timeEnterCurrentStep = stime;
			_currentStepDuration= duration;
			DBG_PRINTF("estimate step:%d, time:%d, duration:%d\n",_currentStep,_timeEnterCurrentStep,_currentStepDuration);
			return;
		}
		stime =next;
	}
	_currentStep=_numberOfSteps;
*/
}

void BrewProfile::_toNextStep(unsigned long time)
{
	do{
		_currentStep++;
		_timeEnterCurrentStep=time;
		if(_currentStep < _numberOfSteps)
			_currentStepDuration =(time_t)(_steps[_currentStep].days * 86400);
	}while(_currentStepDuration == 0 && _currentStep < _numberOfSteps );
	_saveBrewingStatus();
	DBG_PRINTF("_toNextStep:%d current:%ld, duration:%ld\n",_currentStep,time, _currentStepDuration );
}

float BrewProfile::tempByTimeGravity(unsigned long time,Gravity gravity)
{
	if(time < _startDay) return INVALID_CONTROL_TEMP;

	if(	_currentStep==0 && _timeEnterCurrentStep==0){
		_estimateStep(time);
	}
	if(_currentStep >= _numberOfSteps) return INVALID_CONTROL_TEMP;

	DBG_PRINTF("tempByTimeGravity:now:%ld, step:%d, type=%c, last elapsed:%ld\n",time,_currentStep,_steps[_currentStep].condition,time - _timeEnterCurrentStep);

    if(_steps[_currentStep].condition == 'r' ||
    	_steps[_currentStep].condition == 't'){
    	if(_currentStepDuration <= (time - _timeEnterCurrentStep)){
    		// advance to next stage
    		_toNextStep(time);
    	}
    }else{

    	bool sgCondition=(IsGravityValid(gravity))? (gravity <= _steps[_currentStep].sg):false;

    	DBG_PRINTF("tempByTimeGravity: sgC:%c,gravity=%d, target=%d\n",sgCondition? 'Y':'N',gravity,_steps[_currentStep].sg);
/*
   <option value="t">Time</option>
   <option value="g">SG</option>
   <option value="s">Stable</option>
   <option value="a">Time & SG</option>
   <option value="o">Time OR SG</option>
   <option value="u">Time OR Stable</option>
   <option value="v">Time & Stable</option>
    <option value="b">SG OR Stable</option>
    <option value="x">SG & Stable</option>
    <option value="w">ALL</option>
    <option value="e">Either</option>
*/
    #define TIME (_currentStepDuration <= (time - _timeEnterCurrentStep))
    #define STABLE gravityTracker.stable(_steps[_currentStep].stableTime)

    	if(_steps[_currentStep].condition == 'g'){
    		if(sgCondition){
    			_toNextStep(time);
    		}
    	}else if(_steps[_currentStep].condition == 'a'){
    		if(TIME && sgCondition){
    	   		_toNextStep(time);
    		}
    	}else if(_steps[_currentStep].condition == 'o'){
    		if(TIME || sgCondition){
    	   		_toNextStep(time);
    		}
		}else if(_steps[_currentStep].condition == 's'){ // stable
		    if(STABLE ){
    		    _toNextStep(time);
		    }
		}else if(_steps[_currentStep].condition == 'u'){ // time || stable
		    if(TIME  || STABLE ){
    		    _toNextStep(time);
		    }
		}else if(_steps[_currentStep].condition == 'v'){ // time && stable
		    if(TIME  && STABLE ){
    		    _toNextStep(time);
		    }
		}else if(_steps[_currentStep].condition == 'b'){ // sg || stable
		    if(sgCondition  || STABLE ){
    		    _toNextStep(time);
		    }
		}else if(_steps[_currentStep].condition == 'x'){ // sg && stable
		    if(sgCondition  && STABLE ){
    		    _toNextStep(time);
		    }
		}else if(_steps[_currentStep].condition == 'w'){ // time && sg && stable
		    if(sgCondition  && TIME && STABLE ){
    		    _toNextStep(time);
		    }
		}else if(_steps[_currentStep].condition == 'e'){ // time || sg || stable
		    if(sgCondition  || TIME || STABLE ){
    		    _toNextStep(time);
		    }
		}
	}

	if(_currentStep >= _numberOfSteps) return INVALID_CONTROL_TEMP;

	if(_steps[_currentStep].condition == 'r'){
		// ramping
		if(_currentStep ==0 || _currentStep >= (_numberOfSteps-1))
			return INVALID_CONTROL_TEMP;

		float prevTemp= _steps[_currentStep-1].temp;
		float nextTemp= _steps[_currentStep+1].temp;

		float interpolatedTemp = ((float)(time - _timeEnterCurrentStep) /(float)(_currentStepDuration) * (nextTemp - prevTemp) + prevTemp);
    	interpolatedTemp = roundf(interpolatedTemp*10.0)/10.0;

    	return interpolatedTemp;
    }else{
	    return _steps[_currentStep].temp;
	}
}

#else // #if EnableGravitySchedule
void BrewProfile::_tempConvert(void)
{
	for(int i=0;i< _numberOfSteps;i++){
		_setTemps[i] = (_unit == 'C')? F2C(_setTemps[i]):C2F(_setTemps[i]);
	}
}

void BrewProfile::setUnit(char unit)
{
	if(_unit == unit) return;
	_unit = unit;

	if(!_profileLoaded) return;
	_tempConvert();
}
time_t tm_to_timet(struct tm *tm_time);

bool BrewProfile::load(String filename)
{
	DBG_PRINTF("BrewProfile::load\n");


	if(!SPIFFS.exists(filename)){
		DBG_PRINTF("file:%s not exist\n",filename.c_str());
		return false;
	}
	File pf=SPIFFS.open(filename,"r");
	if(!pf){
		DBG_PRINTF("%s open failed\n",filename);
		return false;
	}
	char profileBuffer[MAX_PROFILE_LEN];
	size_t len=pf.readBytes(profileBuffer,MAX_PROFILE_LEN);
	profileBuffer[len]='\0';

	DynamicJsonBuffer jsonBuffer(PROFILE_JSON_BUFFER_SIZE);
	JsonObject& root = jsonBuffer.parseObject(profileBuffer);

	if(!root.success()){
		DBG_PRINTF("JSON parsing failed\n");
		return false;
	}
	if(!root.containsKey("s")
		|| !root.containsKey("u")
		|| !root.containsKey("t")){
		DBG_PRINTF("JSON file not include necessary fields\n");
		return false;
	}
	if (!root["t"].is<JsonArray&>()){
		DBG_PRINTF("JSON t is not array\n");
		return false;
	}
	// get starting time
	//ISO time:
	//2016-07-01T05:22:33.351Z
	//01234567890123456789
	tm tmStart;
	char buf[8];
	const char* sdutc=root["s"];

	#define GetValue(d,s,l) strncpy(buf,sdutc+s,l);buf[l]='\0';d=atoi(buf)
	GetValue(tmStart.tm_year,0,4);
	tmStart.tm_year -= 1970; //1900;
	GetValue(tmStart.tm_mon,5,2);
//	tmStart.tm_mon -= 1;
	GetValue(tmStart.tm_mday,8,2);
	GetValue(tmStart.tm_hour,11,2);
	GetValue(tmStart.tm_min,14,2);
	GetValue(tmStart.tm_sec,17,2);

	DBG_PRINTF("%d/%d/%d %d:%d:%d\n",tmStart.tm_year,tmStart.tm_mon,tmStart.tm_mday,
		tmStart.tm_hour,tmStart.tm_min,tmStart.tm_sec);

	//_startDay = mktime(&tmStart);

	_startDay= tm_to_timet(&tmStart);

	JsonArray& schedule = root["t"];
	_numberOfSteps=schedule.size();

	if(!_setTemps) free(_setTemps);
	_setTemps =(float*) malloc(sizeof(float) * _numberOfSteps);
	if(!_times) free(_times);
	_times = (time_t*)malloc(sizeof(time_t) * _numberOfSteps);
	int i=0;
	for(JsonArray::iterator it=schedule.begin(); it!=schedule.end(); ++it,i++){
		JsonObject&	 entry= *it;
		*(_setTemps + i) = entry["t"];
		float day= entry["d"];
		*(_times+i) =(time_t)(day * 86400.0) + _startDay;
		DBG_PRINTF("%d ,time:%ld temp:",i,*(_times+i) );
		DBG_PRINT(*(_setTemps + i));
		DBG_PRINTF("\n");
	}
	_profileLoaded=true;

	// unit
	const char *punit = root["u"];
	char unit = *punit;
	if(_unit != 'U' && _unit != unit){ // been set by controller
		_unit = unit;
		_tempConvert();
	}else
		_unit = unit;

	DBG_PRINTF("finished, st:%ld, unit:%c, _numberOfSteps:%d\n",_startDay,unit,_numberOfSteps);
	return true;
}

float BrewProfile::tempByTime(unsigned long time)
{
//	DBG_PRINTF("now= st:%ld\n",time);

//	DBG_PRINTF("tempByTime:now:%ld, _startDay:%ld, last time:%ld\n",time,_startDay,*(_times+_numberOfSteps-1));

	if(time < _startDay) return INVALID_CONTROL_TEMP;
	if(time >= *(_times+_numberOfSteps-1)) return INVALID_CONTROL_TEMP;

	int i=1;
	while( time > *(_times+i)) i++;
	float prevTemp= *(_setTemps + i -1);
	float nextTemp= *(_setTemps + i);
	time_t prevDate = *(_times+i-1);
	time_t nextDate =*(_times+i);

	float interpolatedTemp = ((float)(time - prevDate) /(float)(nextDate - prevDate) * (nextTemp - prevTemp) + prevTemp);
    interpolatedTemp = roundf(interpolatedTemp*10.0)/10.0;
    return interpolatedTemp;
}

#endif // #if EnableGravitySchedule

/*
 * Reconstitute "struct tm" elements into a time_t count value.
 * Note that the year argument is offset from 1970.
 */
#define SECS_PER_MIN  (60UL)
#define SECS_PER_HOUR (3600UL)
#define SECS_PER_DAY  (SECS_PER_HOUR * 24UL)
#define DAYS_PER_WEEK (7UL)
#define SECS_PER_WEEK (SECS_PER_DAY * DAYS_PER_WEEK)
#define SECS_PER_YEAR (SECS_PER_WEEK * 52UL)
#define SECS_YR_2000  (946684800UL)	// The time_t value at the very start of Y2K.
static	const uint8_t monthDays[]={31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
#define LEAP_YEAR(Y)		 ( ((1970+Y)>0) && !((1970+Y)%4) && ( ((1970+Y)%100) || !((1970+Y)%400) ) )
time_t tm_to_timet(struct tm *tm_time){

	int i;
	time_t seconds;

	seconds= tm_time->tm_year*(SECS_PER_DAY * 365);
	for (i = 0; i < tm_time->tm_year; i++) {
		if (LEAP_YEAR(i)) {
			seconds += SECS_PER_DAY;	// Add extra days for leap years.
		}
	}
	// Add the number of elapsed days for the given year. Months start from 1.
	for (i = 1; i < tm_time->tm_mon; i++) {
		if ( (i == 2) && LEAP_YEAR(tm_time->tm_year)) {
			seconds += SECS_PER_DAY * 29;
		} else {
			seconds += SECS_PER_DAY * monthDays[i-1];	// "monthDay" array starts from 0.
		}
	}
	seconds+= (tm_time->tm_mday-1) * SECS_PER_DAY;		// Days...
	seconds+= tm_time->tm_hour * SECS_PER_HOUR;		// Hours...
	seconds+= tm_time->tm_min * SECS_PER_MIN;		// Minutes...
	seconds+= tm_time->tm_sec;				// ...and finally, Seconds.
	return (time_t)seconds;
}
// got from https://github.com/PaulStoffregen/Time
void makeTime(time_t timeInput, struct tm &tm){
// break the given time_t into time components
// this is a more compact version of the C library localtime function
// note that year is offset from 1970 !!!

  uint8_t year;
  uint8_t month, monthLength;
  uint32_t time;
  unsigned long days;

  time = (uint32_t)timeInput;
  tm.tm_sec = time % 60;
  time /= 60; // now it is minutes
  tm.tm_min = time % 60;
  time /= 60; // now it is hours
  tm.tm_hour = time % 24;
  time /= 24; // now it is days
  tm.tm_wday = ((time + 4) % 7) + 1;  // Sunday is day 1

  year = 0;
  days = 0;
  while((unsigned)(days += (LEAP_YEAR(year) ? 366 : 365)) <= time) {
    year++;
  }
  tm.tm_year = year +1970; // year is offset from 1970

  days -= LEAP_YEAR(year) ? 366 : 365;
  time  -= days; // now it is days in this year, starting at 0

  days=0;
  month=0;
  monthLength=0;
  for (month=0; month<12; month++) {
    if (month==1) { // february
      if (LEAP_YEAR(year)) {
        monthLength=29;
      } else {
        monthLength=28;
      }
    } else {
      monthLength = monthDays[month];
    }

    if (time >= monthLength) {
      time -= monthLength;
    } else {
        break;
    }
  }
  tm.tm_mon = month + 1;  // jan is month 1
  tm.tm_mday = time + 1;     // day of month
}
